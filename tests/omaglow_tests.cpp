#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <QSet>

#include <QMutex>
#include <QSettings>
#include <memory>
#include <sys/file.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>

#include "backend.h"
#include "design.h"
#include "device.h"
#include "lightservice.h"
#include "filepicker.h"
#include "keymap.h"
#include "clockservice.h"
#include "spectrum.h"
#include "musicservice.h"
#include "oledimage.h"
#include "protocol.h"
#include "razer.h"
#include "laptop.h"
#include "appinfo.h"
#include "theme.h"
#include "translator.h"

// Echoes every command like the keyboard does, and answers the queries.
class FakeLink : public Link {
public:
    QList<QByteArray> sent;
    QList<QByteArray> *log = nullptr; // outlives the link, for services that own it
    bool reject = false;
    bool asleep = false; // behind the receiver, only the receiver answers
    bool mute = false;   // plugged in but not answering at all
    int timeoutMs = 400; // what the backend last set
    int dropNext = 0;    // this many replies are lost, then it answers again
    void setTimeoutMs(int ms) override { timeoutMs = ms; }
    bool checkLock = false; // count reports sent without holding the keyboard lock
    QList<qint64> *times = nullptr; // when each report went out, in ms
    int *unlocked = nullptr;
    int reportId = 0;
    QMutex mutex;

    int posted = 0;
    bool post(const QByteArray &packet) override {
        ++posted;
        return !transact(packet).isEmpty();
    }

    QByteArray transact(const QByteArray &packet) override {
        QMutexLocker lock(&mutex);
        sent.append(packet);
        if (log)
            log->append(packet);
        if (times) {
            static QElapsedTimer clock;
            if (!clock.isValid())
                clock.start();
            times->append(clock.elapsed());
        }
        if (checkLock && unlocked) {
            // Another holder (a separate open of the file) can't take it while it is held.
            const int fd = ::open(qPrintable(DeviceLock::path()), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
            if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
                ++*unlocked;
                ::flock(fd, LOCK_UN);
            }
            ::close(fd);
        }
        if (mute)
            return {};
        if (dropNext > 0) {
            --dropNext;
            return {};
        }
        if (asleep)
            return packet.startsWith(QByteArray::fromHex("1200"))
                ? QByteArray::fromHex("1200000008000700000000000000") : QByteArray::fromHex("ffaa");
        if (reject)
            return QByteArray::fromHex("ffaa");
        if (packet.startsWith(QByteArray::fromHex("1200")))
            return reportId ? QByteArray::fromHex("120000000800070006040618000400") : QByteArray::fromHex("1200000018000400");
        if (packet.startsWith(QByteArray::fromHex("1201")))
            return QByteArray::fromHex("12010000006402010114649a0f");
        return packet;
    }
};

class FakePicker : public FilePicker {
public:
    int opened = 0;
    void openImage() override { ++opened; }
};

static QByteArray hex(const char *text)
{
    return Protocol::report(QByteArray::fromHex(QByteArray(text).replace(' ', "")));
}

// Unpadded bytes, for comparing the start of a packet.
static QByteArray raw(const char *text)
{
    return QByteArray::fromHex(QByteArray(text).replace(' ', ""));
}

static void fakeHidraw(const QString &sys, const QString &dev, const QString &name,
                       const QString &id, const char *descriptor)
{
    QDir().mkpath(sys + "/" + name + "/device");
    QFile uevent(sys + "/" + name + "/device/uevent");
    QVERIFY(uevent.open(QIODevice::WriteOnly));
    uevent.write(("DRIVER=hid-generic\nHID_ID=" + id + "\n").toUtf8());
    QFile report(sys + "/" + name + "/device/report_descriptor");
    QVERIFY(report.open(QIODevice::WriteOnly));
    report.write(QByteArray::fromHex(descriptor));
    QFile node(dev + "/" + name);
    QVERIFY(node.open(QIODevice::WriteOnly));
}

// Each backend gets its own settings file and a fake keyboard that is
// always found; the backend owns the link, the test keeps a pointer to it.
static std::unique_ptr<Backend> makeBackend(QTemporaryDir &dir, FakeLink **link, FakePicker **pickerOut = nullptr)
{
    auto *settings = new QSettings(dir.path() + "/settings.ini", QSettings::IniFormat);
    auto *picker = new FakePicker;
    auto backend = std::make_unique<Backend>(
        [] { return DeviceNode{DeviceNode::Found, "/dev/null", "1A83"}; },
        [link](const QString &) -> Link * { *link = new FakeLink; return *link; },
        settings, picker);
    settings->setParent(backend.get());
    picker->setParent(backend.get());
    if (pickerOut)
        *pickerOut = picker;
    return backend;
}

// A Razer device that answers each feature report with a reply status.
class FakeFeaturePort : public Razer::FeaturePort {
public:
    QList<QByteArray> sent;
    int status = 0x02; // successful
    bool set(const QByteArray &report) override { sent.append(report); return true; }
    QByteArray get() override {
        if (sent.isEmpty())
            return {};
        QByteArray reply = sent.last();
        reply[0] = char(status);
        return reply;
    }
};

// Hands reports to a port the test keeps, since the backend owns what it opens.
class ForwardingPort : public Razer::FeaturePort {
public:
    explicit ForwardingPort(Razer::FeaturePort *to) : m_to(to) {}
    bool set(const QByteArray &report) override { return m_to->set(report); }
    QByteArray get() override { return m_to->get(); }

private:
    Razer::FeaturePort *m_to;
};

class OmaglowTests : public QObject {
    Q_OBJECT

private slots:
    // Never share the keyboard lock with a light service running on this machine.
    void initTestCase()
    {
        static QTemporaryDir runtime;
        qputenv("XDG_RUNTIME_DIR", runtime.path().toLocal8Bit());
    }

    // Packets are compared with what Armoury Crate sent in USB captures.
    void staticColorMatchesCapture()
    {
        Protocol::Lighting s;
        s.effect = Protocol::Static;
        s.brightness = 100;
        s.color1 = QColor("#ff00fd");
        const auto packets = Protocol::lighting(s);
        QCOMPARE(packets.size(), 1);
        QCOMPARE(packets[0], hex("512c0000ff6400ffffff00fd"));
        QCOMPARE(packets[0].size(), 64);
    }

    void breathingModesMatchCapture()
    {
        Protocol::Lighting s;
        s.effect = Protocol::Breathing;
        s.brightness = 50;
        s.color1 = QColor("#ff0000");
        s.color2 = QColor("#ff00ff");
        s.colorMode = Protocol::Double;
        QCOMPARE(Protocol::lighting(s)[0], hex("512c01001e3210ffffff0000ff00ff"));
        s.colorMode = Protocol::Random;
        QCOMPARE(Protocol::lighting(s)[0], hex("512c01001e3201ffffff0000ff00ff"));
        s.colorMode = Protocol::Single;
        s.color1 = QColor("#ffffff");
        QCOMPARE(Protocol::lighting(s)[0], hex("512c01001e3200ffffffffff"));
        s.speed = 0;
        QCOMPARE(static_cast<unsigned char>(Protocol::lighting(s)[0][4]), 0x0f);
        s.speed = 2;
        QCOMPARE(static_cast<unsigned char>(Protocol::lighting(s)[0][4]), 0x2a);
    }

    // Direct mode, verified on the keyboard: count, then up to 15 LED+RGB.
    void directFramePacksFifteenLedsPerReport()
    {
        QList<QPair<int, QColor>> leds;
        for (int i = 0; i < 16; ++i)
            leds.append({i, QColor(i, 0x10, 0x20)});
        const auto packets = Protocol::directFrame(leds);
        QCOMPARE(packets.size(), 2);
        // The third byte counts the LEDs still to come, this packet's included,
        // as Armoury sends it (0x60, 0x51, ... for its 96).
        QVERIFY(packets[0].startsWith(raw("c0811000 00001020 01011020")));
        QCOMPARE(static_cast<unsigned char>(packets[0][4 + 14 * 4]), 14);
        QCOMPARE(packets[1], Protocol::report(raw("c0810100 0f0f1020")));
        QCOMPARE(packets[0].size(), 64);
        QVERIFY(Protocol::directFrame({}).isEmpty());
    }

    // The LED index is column*8+row on the keyboard's matrix (WASD tested).
    // Seen on the keyboard: direct mode answers c0 82, everything else echoes.
    void repliesAreMatchedToTheirCommand()
    {
        QVERIFY(HidrawLink::isReplyTo(raw("c0810100"), raw("c0820100")));
        QVERIFY(HidrawLink::isReplyTo(raw("5100000001"), raw("5100000001")));
        QVERIFY(HidrawLink::isReplyTo(raw("5100"), raw("ffaa")));
        QVERIFY(!HidrawLink::isReplyTo(raw("5100"), raw("1200")));
        QVERIFY(!HidrawLink::isReplyTo(raw("6100"), raw("6200")));

        // A direct frame is written without waiting: the keyboard leaves
        // most of its reports unanswered.
        int fds[2];
        QCOMPARE(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fds), 0); // hidraw nodes are opened non-blocking too
        {
            HidrawLink link(fds[0], 0, 400);
            QElapsedTimer timer;
            timer.start();
            for (int i = 0; i < 6; ++i)
                QVERIFY(link.post(Protocol::report(raw("c0815100"))));
            QVERIFY(timer.elapsed() < 200);
            char buffer[64];
            QCOMPARE(int(::read(fds[1], buffer, sizeof buffer)), 64);
            QVERIFY(Protocol::isDirect(QByteArray(buffer, 64)));
            QVERIFY(!Protocol::isDirect(raw("5100")));
        }
        ::close(fds[1]);
    }

    void framesFollowHowColorsLookOnScreen()
    {
        // LEDs are linear: without a gamma curve dark reds look almost as
        // bright as full red, so waves in one hue don't show.
        auto ledOf = [](const QColor &color) {
            const QList<QByteArray> packets = Design::packets({{0x6e, color}}, 100);
            return QColor(static_cast<unsigned char>(packets[0][5]), static_cast<unsigned char>(packets[0][6]),
                          static_cast<unsigned char>(packets[0][7]));
        };
        QCOMPARE(ledOf(QColor("#ff0000")), QColor("#ff0000"));
        QCOMPARE(ledOf(QColor("#000000")), QColor("#000000"));
        QVERIFY(ledOf(QColor("#4e0300")).red() < 20);
        QVERIFY(ledOf(QColor("#808080")).green() < 60);
    }

    void layersAreStoredWithTheLighting()
    {
        QTemporaryDir dir;
        QSettings settings(dir.path() + "/settings.ini", QSettings::IniFormat);
        Design::ProfileLighting lighting;
        lighting.custom = true;
        lighting.keys = {{0x12, QColor("#ff0000")}};
        Design::Layer wasd;
        wasd.name = "WASD | fast";
        wasd.effect = Design::Strobing;
        wasd.speed = 0;
        wasd.color1 = QColor("#00ff00");
        wasd.color2 = QColor("#0000ff");
        wasd.randomColors = true;
        wasd.keys = {0x12, 0x1f};
        lighting.layers = {wasd, Design::Layer{}};
        Design::storeAt(settings, "profile2/", lighting);
        Design::storeLooks(settings, {{"Party", lighting}});

        for (const Design::ProfileLighting &read : {Design::load(settings, 2), Design::loadLooks(settings).value(0).lighting}) {
            QCOMPARE(read.layers.size(), 2);
            const Design::Layer &l = read.layers[0];
            QCOMPARE(l.name, QString("WASD | fast"));
            QCOMPARE(l.effect, int(Design::Strobing));
            QCOMPARE(l.speed, 0);
            QCOMPARE(l.color1, QColor("#00ff00"));
            QCOMPARE(l.color2, QColor("#0000ff"));
            QVERIFY(l.randomColors);
            QCOMPARE(l.keys, (QList<int>{0x12, 0x1f}));
            QCOMPARE(read.layers[1].effect, int(Design::StaticLayer));
            QVERIFY(read.layers[1].keys.isEmpty());
        }
        // Profiles from before layers have none.
        QVERIFY(Design::load(settings, 3).layers.isEmpty());
    }

    void layersCoverEarlierOnesInOrder()
    {
        Design::ProfileLighting lighting;
        lighting.custom = true;
        lighting.keys = {{0x12, QColor("#ff0000")}, {0x6e, QColor("#ffffff")}};
        Design::Layer blue;
        blue.color1 = QColor("#0000ff");
        blue.keys = {0x12, 0x1f};
        Design::Layer green;
        green.color1 = QColor("#00ff00");
        green.keys = {0x1f};
        lighting.layers = {blue, green};
        const Design::Keys frame = Design::render(lighting, 1234);
        QCOMPARE(frame.value(0x12), QColor("#0000ff"));
        QCOMPARE(frame.value(0x1f), QColor("#00ff00"));
        QCOMPARE(frame.value(0x6e), QColor("#ffffff")); // painted, in no layer
        QVERIFY(!frame.contains(0x20));
        // Static layers don't need streaming; any other effect does.
        QVERIFY(!Design::animated(lighting));
        lighting.layers[1].effect = Design::Breathing;
        QVERIFY(Design::animated(lighting));
        lighting.layers[1].keys.clear();
        QVERIFY(!Design::animated(lighting));
        lighting.layers[1].keys = {0x1f};
        lighting.custom = false;
        QVERIFY(!Design::animated(lighting));
    }

    void newLayerEffectsDrawTheirShapes()
    {
        Design::ProfileLighting lighting;
        lighting.custom = true;
        Design::Layer layer;
        layer.color1 = QColor("#ff0000");
        layer.color2 = QColor("#ffff00");
        layer.speed = 1; // a 3 second cycle
        for (const auto &k : Keymap::layout())
            layer.keys.append(k.code);
        lighting.layers = {layer};
        auto at = [&](int effect, qint64 ms) {
            lighting.layers[0].effect = effect;
            return Design::render(lighting, ms);
        };
        auto animatedAs = [&](int effect) {
            lighting.layers[0].effect = effect;
            return Design::animated(lighting);
        };
        const int esc = 0x6e, right = 0x59, space = 0x2c;

        // Fire rises from the space bar: the bottom burns brighter than the
        // top row over a cycle, and the flames flicker.
        int bottom = 0, top = 0;
        for (qint64 ms = 0; ms < 3000; ms += 100) {
            bottom += at(Design::Fire, ms).value(space).value();
            top += at(Design::Fire, ms).value(esc).value();
        }
        QVERIFY(bottom > top * 2);
        QVERIFY(at(Design::Fire, 0) != at(Design::Fire, 400));

        // Plasma blends the colors across the keys and keeps moving.
        QVERIFY(at(Design::Plasma, 0).value(esc) != at(Design::Plasma, 0).value(right));
        QVERIFY(at(Design::Plasma, 0) != at(Design::Plasma, 1000));

        // The scanner is a narrow bar going back and forth: Esc and the right
        // arrow are never lit together, and each is lit at some point.
        bool escLit = false, rightLit = false;
        for (qint64 ms = 0; ms < 3000; ms += 50) {
            const Design::Keys frame = at(Design::Scanner, ms);
            const bool e = frame.value(esc).value() > 0, r = frame.value(right).value() > 0;
            QVERIFY(!(e && r));
            escLit |= e;
            rightLit |= r;
        }
        QVERIFY(escLit && rightLit);

        // Fireflies glow here and there, the same at the same moment.
        int lit = 0, dark = 0;
        for (const QColor &c : at(Design::Fireflies, 5000))
            (c.value() > 0 ? lit : dark)++;
        QVERIFY(lit > 0 && dark > lit);
        QCOMPARE(at(Design::Fireflies, 5000), at(Design::Fireflies, 5000));

        // A heartbeat: a full beat, a softer second one, then a dim rest.
        const int first = at(Design::Heartbeat, 3000 * 6 / 100).value(esc).value();
        const int second = at(Design::Heartbeat, 3000 * 26 / 100).value(esc).value();
        const int rest = at(Design::Heartbeat, 3000 * 70 / 100).value(esc).value();
        QCOMPARE(first, 255);
        QVERIFY(second < first && second > rest);
        QVERIFY(rest < 40);

        // The gradient goes from the first color on the left to the second on
        // the right, and stays still, so it isn't streamed.
        QCOMPARE(at(Design::Gradient, 0).value(esc), QColor("#ff0000"));
        QCOMPARE(at(Design::Gradient, 0).value(right), QColor("#ffff00"));
        QCOMPARE(at(Design::Gradient, 0), at(Design::Gradient, 2000));
        QVERIFY(!animatedAs(Design::Gradient));
        QVERIFY(animatedAs(Design::Fire));

        // Every effect has a name for Studio.
        QCOMPARE(int(Design::LayerEffectCount), 15);
    }

    void layerEffectsMoveOverTime()
    {
        Design::ProfileLighting lighting;
        lighting.custom = true;
        Design::Layer layer;
        layer.color1 = QColor("#ff0000");
        layer.color2 = QColor("#0000ff");
        layer.speed = 1; // a 3 second cycle
        for (const auto &k : Keymap::layout())
            layer.keys.append(k.code);
        lighting.layers = {layer};
        auto at = [&](int effect, qint64 ms) {
            lighting.layers[0].effect = effect;
            return Design::render(lighting, ms);
        };
        const int esc = 0x6e, right = 0x59;

        QCOMPARE(at(Design::Breathing, 0).value(esc), QColor(0, 0, 0));
        QCOMPARE(at(Design::Breathing, 1500).value(esc), QColor("#ff0000"));
        QCOMPARE(at(Design::Breathing, 4500).value(esc), QColor("#0000ff")); // the next breath, second color

        QCOMPARE(at(Design::Strobing, 0).value(esc), QColor("#ff0000"));
        QCOMPARE(at(Design::Strobing, 700).value(esc), QColor(0, 0, 0));

        QVERIFY(at(Design::ColorCycle, 0).value(esc) != at(Design::ColorCycle, 1000).value(esc));
        QCOMPARE(at(Design::ColorCycle, 500).value(esc), at(Design::ColorCycle, 500).value(right));

        QVERIFY(at(Design::Rainbow, 0).value(esc) != at(Design::Rainbow, 0).value(right));
        QVERIFY(at(Design::Rainbow, 0).value(esc) != at(Design::Rainbow, 1000).value(esc));

        QVERIFY(at(Design::Tide, 0).value(esc) != at(Design::Tide, 750).value(esc));

        // The comet crosses the keyboard: Esc lights before the arrows do.
        bool escFirst = false;
        for (qint64 ms = 0; ms < 3000; ms += 50) {
            const Design::Keys frame = at(Design::Comet, ms);
            if (frame.value(esc).value() > 0 && frame.value(right).value() == 0) {
                escFirst = true;
                break;
            }
            QVERIFY(frame.value(right).value() == 0);
        }
        QVERIFY(escFirst);

        // Stars come and go on their own: some lit, some dark, and the same
        // moment always looks the same.
        int lit = 0, dark = 0;
        for (const QColor &c : at(Design::StarryNight, 2000))
            (c.value() > 0 ? lit : dark)++;
        QVERIFY(lit > 0 && dark > 0);
        QCOMPARE(at(Design::StarryNight, 2000), at(Design::StarryNight, 2000));

        // The current runs through the keys row by row: a short lit stretch
        // that is somewhere else a moment later.
        auto litKeys = [&](qint64 ms) {
            QSet<int> lit;
            const Design::Keys frame = at(Design::Current, ms);
            for (auto it = frame.begin(); it != frame.end(); ++it)
                if (it.value().value() > 0)
                    lit.insert(it.key());
            return lit;
        };
        const QSet<int> early = litKeys(300), later = litKeys(1500);
        QVERIFY(!early.isEmpty() && early.size() <= 8);
        QVERIFY(!later.isEmpty() && later.size() <= 8);
        QVERIFY(early != later);

        // Brightness scales what is sent, not what is drawn.
        QCOMPARE(at(Design::StaticLayer, 0).value(esc), QColor("#ff0000"));
    }

    void keysHaveTheirLed()
    {
        QCOMPARE(Keymap::ledOf(0x12), 0x12); // W
        QCOMPARE(Keymap::ledOf(0x1f), 0x0b); // A
        QCOMPARE(Keymap::ledOf(0x20), 0x13); // S
        QCOMPARE(Keymap::ledOf(0x21), 0x1b); // D
        QCOMPARE(Keymap::ledOf(0x6e), 0x00); // Esc
        QCOMPARE(Keymap::ledOf(0x3d), 0x35); // Space
        QCOMPARE(Keymap::ledOf(0x59), 0x7d); // Right arrow
        QCOMPARE(Keymap::ledOf(0x9f), 0x5d); // Fn
        QCOMPARE(Keymap::ledOf(0x999), -1);
        QSet<int> seen;
        for (const auto &k : Keymap::layout()) {
            const int led = Keymap::ledOf(k.code);
            QVERIFY2(led >= 0 && led < 128, k.label);
            seen.insert(led);
        }
        QCOMPARE(seen.size(), Keymap::layout().size());
    }

    void colorCycleMatchesCapture()
    {
        Protocol::Lighting s;
        s.effect = Protocol::ColorCycle;
        s.brightness = 50;
        QCOMPARE(Protocol::lighting(s)[0], hex("512c02001f3200ffff000000"));
    }

    void rainbowMatchesCapture()
    {
        Protocol::Lighting s;
        s.effect = Protocol::Rainbow;
        s.brightness = 50;
        const auto packets = Protocol::lighting(s);
        QCOMPARE(packets.size(), 3);
        QCOMPARE(packets[0], hex("512c04023e320000 02070ef500ff1d0006ff"));
        QCOMPARE(packets[1], hex("512c04012b00faff3901ff0048fff60056ff7807"));
        QCOMPARE(packets[2], hex("512c040064ff000d"));
        s.direction = 7;
        s.thickness = 0;
        s.speed = 0;
        QCOMPARE(Protocol::lighting(s)[0], hex("512c040223320007 00070ef500ff1d0006ff"));
    }

    void rippleAndQuicksandMatchCapture()
    {
        Protocol::Lighting s;
        s.brightness = 50;
        s.effect = Protocol::Ripple;
        QCOMPARE(Protocol::lighting(s)[0], hex("512c05026432 01ff02070ef500ff1d0006ff"));
        s.effect = Protocol::Quicksand;
        const auto packets = Protocol::lighting(s);
        QCOMPARE(packets.size(), 2);
        QCOMPARE(packets[0], hex("512c07010f320102fffff6000 1ff0000faff"));
        QCOMPARE(packets[1], hex("512c07000006fff500ffff000d"));
        s.direction = 6;
        QCOMPARE(static_cast<unsigned char>(Protocol::lighting(s)[0][7]), 0x06);
        s.direction = 3; // not a quicksand direction, so the default comes back
        QCOMPARE(static_cast<unsigned char>(Protocol::lighting(s)[0][7]), 0x02);
    }

    void simpleCommandsMatchCapture()
    {
        QCOMPARE(Protocol::lightingOff(), hex("512c0000ff0400ffff000000"));
        QCOMPARE(Protocol::oledBrightness(75), hex("680000004b"));
        QCOMPARE(Protocol::oledPower(false), hex("6900000000"));
        QCOMPARE(Protocol::oledPower(true), hex("6900000001"));
        QCOMPARE(Protocol::oledPreset(3), hex("6100000003"));
        QCOMPARE(Protocol::save(), hex("5055"));
        QCOMPARE(Protocol::oledBrightness(250), hex("6800000064"));
    }

    void oledUploadMatchesCapture()
    {
        const QByteArray white(Protocol::OledFrameBytes, char(0xff));
        const auto packets = Protocol::oledUpload({white}, {1000});
        QCOMPARE(packets.size(), 1 + 111 + 1);
        QCOMPARE(packets.first(), hex("610100006f00"));
        QVERIFY(packets[1].startsWith(raw("61026e0001 00e803ffff")));
        QVERIFY(packets[111].startsWith(raw("61020000")));
        QCOMPARE(packets.last(), hex("6103"));

        // Two frames at 500 ms: 222 packets, as Armoury sent.
        const auto two = Protocol::oledUpload({white, white}, {500, 500});
        QCOMPARE(two.first(), hex("61010000de00"));
        QVERIFY(two[1].startsWith(raw("6102dd000200f401f401")));

        // Past 255 packets the count and indexes use the high byte.
        const auto three = Protocol::oledUpload({white, white, white}, {100, 100, 100});
        const int count = (6 + 3 * Protocol::OledFrameBytes + 59) / 60;
        QCOMPARE(three.first(), Protocol::report({0x61, 0x01, 0, 0, count & 0xff, count >> 8}));
        QVERIFY(three[1].startsWith(Protocol::report({0x61, 0x02, (count - 1) & 0xff, (count - 1) >> 8}).left(4)));
    }

    void repliesAreParsed()
    {
        QCOMPARE(Protocol::versionFromReply(hex("1200000018000400")), QString("4.00.18"));
        QCOMPARE(Protocol::batteryFromReply(hex("12010000006402010114649a0f")), 100);
        QCOMPARE(Protocol::batteryFromReply(hex("1200")), -1);
        QVERIFY(Protocol::isRejected(hex("ffaa")));
        QVERIFY(!Protocol::isRejected(hex("512c")));
    }

    void discoveryFindsTheVendorInterface()
    {
        QTemporaryDir dir;
        const QString sys = dir.path() + "/sys", dev = dir.path() + "/dev";
        QDir().mkpath(dev);
        fakeHidraw(sys, dev, "hidraw2", "0003:00000B05:00001A83", "05010906a101");  // keyboard
        fakeHidraw(sys, dev, "hidraw5", "0003:00000B05:00001A83", "0600ff0901a101"); // vendor
        fakeHidraw(sys, dev, "hidraw7", "0003:0000046D:0000C52B", "0600ff0901a101"); // someone else

        DeviceNode node = Discovery::find(sys, dev);
        QCOMPARE(node.status, DeviceNode::Found);
        QCOMPARE(node.path, dev + "/hidraw5");
        QCOMPARE(node.productId, QString("1A83"));

        QFile::setPermissions(dev + "/hidraw5", QFile::ReadOwner);
        QCOMPARE(Discovery::find(sys, dev).status, DeviceNode::NoPermission);

        QTemporaryDir empty;
        QCOMPARE(Discovery::find(empty.path(), empty.path()).status, DeviceNode::NotFound);
    }

    void discoveryFindsTheOmniReceiver()
    {
        QTemporaryDir dir;
        const QString sys = dir.path() + "/sys", dev = dir.path() + "/dev";
        QDir().mkpath(dev);
        fakeHidraw(sys, dev, "hidraw4", "0003:00000B05:00001ACE", "05010906a1018501");
        // Report 1 on 0xFF02, report 2 on 0xFF00 (the keyboard), report 3 on 0xFF01.
        fakeHidraw(sys, dev, "hidraw6", "0003:00000B05:00001ACE",
                   "0602ff0901a10185010902750895 3f150026ff00810209037508953f150026ff009102c0"
                   "0600ff0901a10185020902750895 3f150026ff00810209037508953f150026ff009102c0");
        const DeviceNode node = Discovery::find(sys, dev);
        QCOMPARE(node.status, DeviceNode::Found);
        QCOMPARE(node.path, dev + "/hidraw6");
        QCOMPARE(node.productId, QString("1ACE"));
        QCOMPARE(node.reportId, 2);
    }

    void omniFramingAddsAndStripsTheReportId()
    {
        const QByteArray packet = Protocol::report({0x12, 0x01});
        const QByteArray framed = HidrawLink::frame(packet, 2);
        QCOMPARE(framed.size(), 64);
        QCOMPARE(framed.left(3), raw("021201"));
        QCOMPARE(HidrawLink::unframe(raw("0212010000005702"), 2), raw("12010000005702"));
        QCOMPARE(HidrawLink::frame(packet, 0), packet); // the cable has no report ID
    }

    void omniRepliesAreParsed()
    {
        // Through the receiver the version reply carries the receiver's first.
        QCOMPARE(Protocol::versionFromReply(raw("120000000800070006040618000400"), true), QString("4.00.18"));
        QCOMPARE(Protocol::batteryFromReply(raw("12010000005702010014579a0f")), 0x57);
        // A sleeping keyboard behind the receiver: only the receiver answers.
        QCOMPARE(Protocol::versionFromReply(raw("1200000008000700000000000000"), true), QString());
        QCOMPARE(Discovery::vendorReportId(raw("0600ff0901a10109")), 0);
        QCOMPARE(Discovery::vendorReportId(raw("0602ff0901a1018501c00600ff0901a1018502")), 2);
        QCOMPARE(Discovery::vendorReportId(raw("05010906a101")), -1);
        // Only at a collection boundary, never in the middle of other items.
        QCOMPARE(Discovery::vendorReportId(raw("05010906a1010600ff0901a101")), -1);
    }

    void omniReportsASleepingKeyboard()
    {
        QTemporaryDir dir;
        QSettings settings(dir.path() + "/s.ini", QSettings::IniFormat);
        FakePicker picker;
        FakeLink *link = nullptr;
        Backend backend([] { return DeviceNode{DeviceNode::Found, "/dev/null", "1ACE", 2}; },
                        [&](const QString &) -> Link * {
                            link = new FakeLink;
                            link->asleep = true;
                            link->reportId = 2;
                            return link;
                        }, &settings, &picker);
        QCOMPARE(backend.connection(), int(Backend::Connected));
        QVERIFY(backend.asleep());
        QCOMPARE(backend.firmware(), QString());

        backend.setBrightness(30);
        backend.flush();
        QVERIFY(backend.error().contains("asleep"));

        link->asleep = false; // a key was pressed
        backend.checkWake();
        QVERIFY(!backend.asleep());

        // Falling asleep while the window is open shows up on the next battery check.
        link->asleep = true;
        backend.refreshBattery();
        QVERIFY(backend.asleep());
        link->asleep = false;
        backend.checkWake();
        QVERIFY(!backend.asleep());
        QCOMPARE(backend.firmware(), QString("4.00.18"));
    }

    void oledUploadThroughTheReceiverMatchesCapture()
    {
        // Through the ROG Omni receiver Armoury sends 59 data bytes per packet.
        QByteArray left(Protocol::OledFrameBytes, '\0');
        for (int y = 0; y < Protocol::OledHeight; ++y)
            for (int x = 0; x < 52; ++x)
                left[y * Protocol::OledBytesPerRow + x] = char(0xff);
        const auto packets = Protocol::oledUpload({left, left}, {500, 500}, Protocol::OledChunkReceiver);
        QCOMPARE(packets.size(), 1 + 226 + 1);
        QCOMPARE(packets.first(), hex("61010000e200"));
        QCOMPARE(packets[1].left(4 + 59), raw("6102e100" "0200f401f401") + QByteArray(52, char(0xff)) + QByteArray(1, '\0'));
        QVERIFY(packets[1].mid(63).count('\0') == 1); // fits the receiver's 63 bytes
        QCOMPARE(packets[226].left(4), raw("61020000"));
    }

    void backendUploadsThroughTheReceiver()
    {
        QTemporaryDir dir;
        QImage picture(208, 64, QImage::Format_RGB32);
        picture.fill(Qt::white);
        QVERIFY(picture.save(dir.path() + "/p.png"));
        QSettings settings(dir.path() + "/s.ini", QSettings::IniFormat);
        FakePicker picker;
        FakeLink *link = nullptr;
        Backend backend([] { return DeviceNode{DeviceNode::Found, "/dev/null", "1ACE", 2}; },
                        [&](const QString &) -> Link * { link = new FakeLink; return link; }, &settings, &picker);
        QVERIFY(backend.canSendPictures());
        link->sent.clear();
        backend.uploadOled(dir.path() + "/p.png");
        QTRY_VERIFY(!backend.busy());
        QCOMPARE(link->sent.first(), hex("610100007100")); // (4 + 6656) / 59 = 113 packets
        QCOMPARE(link->sent.size(), 1 + 113 + 1 + 1);
    }

    void discoveryAcceptsTheDongle()
    {
        QString pid;
        QVERIFY(Discovery::isAzoth("HID_ID=0003:00000B05:00001A85\n", &pid));
        QCOMPARE(pid, QString("1A85"));
        QVERIFY(!Discovery::isAzoth("HID_ID=0003:00000B05:000019B6\n"));
    }

    void oledFramesEncodeAndDecode()
    {
        QImage white(416, 128, QImage::Format_RGB32); // the panel's 3.25:1
        white.fill(Qt::white);
        const QByteArray frame = OledImage::encodeFrame(white);
        QCOMPARE(frame.size(), Protocol::OledFrameBytes);
        QCOMPARE(frame, QByteArray(Protocol::OledFrameBytes, char(0xff)));

        // A square is letterboxed: black on the sides, white in the middle.
        QImage square(64, 64, QImage::Format_RGB32);
        square.fill(Qt::white);
        const QImage decoded = OledImage::decodeFrame(OledImage::encodeFrame(square));
        QCOMPARE(decoded.size(), QSize(208, 64));
        QCOMPARE(qGray(decoded.pixel(0, 32)), 0);
        QCOMPARE(qGray(decoded.pixel(104, 32)), 255);
    }

    void oledLoadsAStillPicture()
    {
        QTemporaryDir dir;
        QImage picture(208, 64, QImage::Format_RGB32);
        picture.fill(Qt::white);
        QVERIFY(picture.save(dir.path() + "/p.png"));
        const auto animation = OledImage::load(dir.path() + "/p.png");
        QVERIFY(animation.error.isEmpty());
        QCOMPARE(animation.frames.size(), 1);
        QCOMPARE(animation.delays, QList<int>{1000});
        QVERIFY(!OledImage::load(dir.path() + "/missing.png").error.isEmpty());
    }

    void remapMatchesCapture()
    {
        // Caps Lock to B, volume up, left click, mute, Copilot, and back.
        QCOMPARE(Protocol::remapKey(0x1e, 0x32, 0x00), hex("51201e003200"));
        QCOMPARE(Protocol::remapKey(0x1e, 0xbb, 0x00), hex("51201e00bb00"));
        QCOMPARE(Protocol::remapKey(0x1e, 0xf0, 0x01), hex("51201e00f001"));
        QCOMPARE(Protocol::remapKey(0x1e, 0xb8, 0x00), hex("51201e00b800"));
        QCOMPARE(Protocol::remapKey(0x1e, 0xd3, 0x00), hex("51201e00d300"));
        QCOMPARE(Protocol::remapKey(0x1e, 0xff, 0x00), hex("51201e00ff00"));
    }

    void knobMatchesCapture()
    {
        QCOMPARE(Protocol::knobFunction(Protocol::KnobMedia, false), hex("710000000100"));
        QCOMPARE(Protocol::knobFunction(Protocol::KnobKeyboardBrightness, true), hex("710000000201"));
        QCOMPARE(Protocol::knobFunction(Protocol::KnobCustom, true), hex("710000000701"));
        QCOMPARE(Protocol::knobCustom(Protocol::KnobScrollUp, 0xd3, 0x00), hex("7101000007a400d300"));
        QCOMPARE(Protocol::knobCustom(Protocol::KnobClick, 0xf0, 0x01), hex("7101000007a500f001"));
        QCOMPARE(Protocol::knobCustom(Protocol::KnobScrollDown, 0xd3, 0x00), hex("7101000007a600d300"));
    }

    void keymapCoversArmourysResetList()
    {
        // The 81 positions Armoury resets, in its order.
        const QList<int> armoury = {0x1e, 0x6e, 0x1f, 0x2e, 0x02, 0x70, 0x3d, 0x2b, 0x59, 0x71, 0x72, 0x73, 0x74,
            0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x7b, 0x01, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
            0x0c, 0x0d, 0x0f, 0x4b, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c,
            0x1d, 0x4c, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x55, 0x2c, 0x2f, 0x30, 0x31,
            0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x39, 0x53, 0x56, 0x3a, 0x3b, 0x3c, 0x3e, 0x9f, 0x40, 0x4f, 0x54};
        QCOMPARE(QSet<int>(armoury.begin(), armoury.end()).size(), 81);
        const QList<int> ours = Keymap::resetCodes();
        QCOMPARE(QSet<int>(ours.begin(), ours.end()), QSet<int>(armoury.begin(), armoury.end()));
        QCOMPARE(QString(Keymap::find(0x29)->label), QString("'"));
        QCOMPARE(QString(Keymap::find(0x28)->label), QString(";"));
        QCOMPARE(Keymap::labelOf({0xbb, 0x00}), QString("Volume up"));
        QCOMPARE(Keymap::kindOf({0x32, 0x00}), Keymap::KeyPress);
    }

    void backendRemapsAndRemembersKeys()
    {
        QTemporaryDir dir;
        {
            FakeLink *link = nullptr;
            auto backend = makeBackend(dir, &link);
            link->sent.clear();
            backend->remap(0x1e, 0x6e, 0x00); // Caps Lock sends Esc
            QCOMPARE(link->sent, (QList<QByteArray>{hex("51201e006e00"), hex("5055")}));
            QCOMPARE(backend->remappedCount(), 1);
            backend->remap(0x99, 0x6e, 0x00); // not a key: nothing sent
            QCOMPARE(link->sent.size(), 2);
        }
        FakeLink *link = nullptr;
        auto backend = makeBackend(dir, &link);
        QCOMPARE(backend->keyAction(0x1e), (Keymap::Action{0x6e, 0x00}));

        link->sent.clear();
        backend->resetKeys();
        QCOMPARE(link->sent.size(), 81 + 1);
        QCOMPARE(link->sent.last(), hex("5055"));
        QVERIFY(link->sent.contains(hex("51201e00ff00")));
        QCOMPARE(backend->remappedCount(), 0);
    }

    void backendConfiguresTheKnob()
    {
        QTemporaryDir dir;
        FakeLink *link = nullptr;
        auto backend = makeBackend(dir, &link);
        QVERIFY(backend->knobEnabled(Protocol::KnobVolume));
        QVERIFY(!backend->knobEnabled(Protocol::KnobCustom));

        link->sent.clear();
        backend->setKnobFunction(Protocol::KnobCustom, true);
        backend->setKnobSlot(Protocol::KnobClick, 0xf0, 0x01);
        QCOMPARE(link->sent, (QList<QByteArray>{hex("710000000701"), hex("5055"),
                                                hex("7101000007a500f001"), hex("5055")}));
        QVERIFY(backend->knobEnabled(Protocol::KnobCustom));
        QCOMPARE(backend->knobSlot(Protocol::KnobClick), (Keymap::Action{0xf0, 0x01}));

        link->reject = true;
        backend->setKnobFunction(Protocol::KnobMedia, false);
        QVERIFY(backend->knobEnabled(Protocol::KnobMedia)); // unchanged when the keyboard refuses
    }

    void powerAndDebounceMatchCapture()
    {
        QCOMPARE(Protocol::sleepTimeout(4), hex("5138000004"));      // 10 minutes
        QCOMPARE(Protocol::sleepTimeout(2), hex("5138000002"));      // 3 minutes
        QCOMPARE(Protocol::sleepTimeout(Protocol::SleepNever), hex("51380000ff"));
        QCOMPARE(Protocol::batteryAlert(50), hex("513a000032"));
        QCOMPARE(Protocol::batteryAlert(20), hex("513a000014"));
        QCOMPARE(Protocol::powerSaving(0), hex("5139000000"));
        QCOMPARE(Protocol::powerSaving(1), hex("5139000001"));
        QCOMPARE(Protocol::powerSaving(2), hex("5139000002"));
        QCOMPARE(Protocol::debounce(20), hex("6c00000014"));
        QCOMPARE(Protocol::debounce(50), hex("6c00000032"));
        QCOMPARE(Protocol::debounce(15), hex("6c0000000f"));
        QCOMPARE(Protocol::debounce(90), hex("6c00000032")); // clamped to Armoury's range
    }

    void backendSendsPowerAndDebounce()
    {
        QTemporaryDir dir;
        {
            FakeLink *link = nullptr;
            auto backend = makeBackend(dir, &link);
            QCOMPARE(backend->sleepTimeout(), 2);  // Armoury's defaults
            QCOMPARE(backend->batteryAlert(), 20);
            QCOMPARE(backend->powerSaving(), 1);
            QCOMPARE(backend->debounceMs(), 15);
            link->sent.clear();
            backend->setSleepTimeout(Protocol::SleepNever);
            backend->setBatteryAlert(50);
            backend->setPowerSaving(2);
            backend->setDebounceMs(25);
            QCOMPARE(link->sent, (QList<QByteArray>{hex("51380000ff"), hex("5055"), hex("513a000032"), hex("5055"),
                                                    hex("5139000002"), hex("5055"), hex("6c00000019"), hex("5055")}));
        }
        FakeLink *link = nullptr;
        auto backend = makeBackend(dir, &link);
        QCOMPARE(backend->sleepTimeout(), int(Protocol::SleepNever));
        QCOMPARE(backend->batteryAlert(), 50);
        QCOMPARE(backend->powerSaving(), 2);
        QCOMPARE(backend->debounceMs(), 25);
    }

    void clockMatchesCapture()
    {
        // Armoury at 2026-11-07 04:46.
        QCOMPARE(Protocol::clockTime(QDateTime(QDate(2026, 11, 7), QTime(4, 46, 30))), hex("6300000001ea070b07042e"));
        QCOMPARE(Protocol::clockTime(QDateTime(QDate(2026, 11, 7), QTime(17, 5))), hex("6300000001ea070b071105"));
    }

    void clockServicePushesTheTimeAndReconnects()
    {
        QDateTime now(QDate(2026, 11, 7), QTime(12, 34, 10));
        bool present = true;
        QList<QByteArray> sent;
        ClockService clock(
            [&] { return present ? DeviceNode{DeviceNode::Found, "/dev/null", "1A83"} : DeviceNode{}; },
            [&](const QString &) -> Link * {
                auto *link = new FakeLink;
                QObject::connect(qApp, &QObject::destroyed, [] {});
                return link;
            },
            [&] { return now; });
        clock.setObserver([&](const QByteArray &p) { sent.append(p); });

        clock.tick();
        QCOMPARE(sent, QList<QByteArray>{hex("6300000001ea070b070c22")});

        // Within the same minute nothing more is sent; a new minute (or a jump
        // after suspend or a clock change) is sent on the next check.
        now = now.addSecs(5);
        clock.tick();
        QCOMPARE(sent.size(), 1);
        now = now.addSecs(3600);
        clock.tick();
        QCOMPARE(sent.last(), hex("6300000001ea070b070d22"));
        now = now.addSecs(-3600);
        sent.removeLast();

        present = false;
        clock.dropLink();
        now = QDateTime(QDate(2026, 11, 7), QTime(12, 35, 10));
        clock.tick();
        QCOMPARE(sent.size(), 1); // no keyboard, nothing sent

        present = true;
        clock.tick();
        QCOMPARE(sent.last(), hex("6300000001ea070b070c23"));
    }

    void backendSwitchesTheClockService()
    {
        QTemporaryDir dir;
        FakeLink *link = nullptr;
        QStringList commands;
        auto backend = makeBackend(dir, &link);
        backend->setServiceRunner([&](const QStringList &args, QString *) { commands.append(args.join(' ')); return 0; });

        link->sent.clear();
        backend->setOledClock(true);
        QVERIFY(backend->oledClock());
        QCOMPARE(commands.last(), QString("--user enable --now omaglow-clock.service"));
        QVERIFY(link->sent.first().startsWith(raw("6300000001"))); // shown right away

        // Picking a preset stops the clock, or it would take the display back.
        backend->chooseOledPreset(2);
        QVERIFY(!backend->oledClock());
        QCOMPARE(commands.last(), QString("--user disable --now omaglow-clock.service"));

        // Turning the display off pauses the service; on again resumes it.
        backend->setOledClock(true);
        backend->setOledOn(false);
        QCOMPARE(commands.last(), QString("--user stop omaglow-clock.service"));
        QVERIFY(backend->oledClock());
        backend->setOledOn(true);
        QCOMPARE(commands.last(), QString("--user start omaglow-clock.service"));
        backend->chooseOledPreset(0);

        // A failing systemctl is reported, the switch stays off, and nothing
        // was sent to the keyboard first.
        link->sent.clear();
        backend->setServiceRunner([](const QStringList &, QString *error) { *error = "no user bus"; return 1; });
        backend->setOledClock(true);
        QVERIFY(!backend->oledClock());
        QVERIFY(backend->error().contains("no user bus"));
        QVERIFY(link->sent.isEmpty());

        // The real service state wins over what was remembered.
        backend->setServiceRunner([](const QStringList &args, QString *) { return args.contains("is-enabled") ? 0 : 1; });
        backend->syncClockService();
        QVERIFY(backend->oledClock());
        backend->setServiceRunner([](const QStringList &, QString *) { return 1; });
        backend->syncClockService();
        QVERIFY(!backend->oledClock());
    }

    // Looks are saved lightings: an effect with its settings, or a design.
    void backendSavesAndAppliesLooks()
    {
        QTemporaryDir dir;
        {
            FakeLink *link = nullptr;
            auto backend = makeBackend(dir, &link);
            backend->setEffect(Protocol::Breathing);
            backend->setColor1(QColor("#ff0000"));
            backend->saveLook("Calm");
            backend->paintKeys({0x12}, QColor("#00ff00"));
            backend->saveLook(" Red WASD ");
            backend->saveLook("");
            QCOMPARE(backend->looks().size(), 2);
            QCOMPARE(backend->looks()[1].toMap().value("name").toString(), QString("Red WASD"));
            QVERIFY(backend->looks()[1].toMap().value("custom").toBool());
            backend->saveLook("calm"); // same name: replaced, not doubled
            QCOMPARE(backend->looks().size(), 2);
            QVERIFY(backend->looks()[0].toMap().value("custom").toBool());

            backend->setEffect(Protocol::Rainbow);
            backend->flush();
            backend->applyLook(1);
            QVERIFY(backend->customLighting());
            QCOMPARE(backend->design().value("18").value<QColor>(), QColor("#00ff00"));
            link->sent.clear();
            backend->flush();
            QVERIFY(link->sent.first().startsWith(raw("c081")));
            backend->deleteLook(0);
            QCOMPARE(backend->looks().size(), 1);
        }
        FakeLink *link = nullptr;
        auto backend = makeBackend(dir, &link);
        QCOMPARE(backend->looks().size(), 1);
        QCOMPARE(backend->looks()[0].toMap().value("name").toString(), QString("Red WASD"));
    }

    void musicPacketsMatchCapture()
    {
        // Armoury's "audio spectrum" mode, bar style, then one frame of bands.
        QCOMPARE(Protocol::musicSpectrumStart(1), hex("670000000101 0000d0004000"));
        QCOMPARE(Protocol::musicSpectrumStart(3), hex("670000000103 0000d0004000"));
        QByteArray bands(26, char(0x40));
        QCOMPARE(Protocol::musicBands(bands), Protocol::report(raw("67020000") + bands));
    }

    void spectrumFindsTheTone()
    {
        const int rate = 44100, n = Spectrum::WindowSize;
        QList<float> silence(n, 0.0f);
        QCOMPARE(Spectrum::bands(silence, rate), QByteArray(Spectrum::BandCount, '\0'));

        auto tone = [&](double hz) {
            QList<float> samples(n);
            for (int i = 0; i < n; ++i)
                samples[i] = 0.5f * float(std::sin(2 * M_PI * hz * i / rate));
            return Spectrum::bands(samples, rate);
        };
        auto loudest = [](const QByteArray &b) {
            int best = 0;
            for (int i = 1; i < b.size(); ++i)
                if (static_cast<unsigned char>(b[i]) > static_cast<unsigned char>(b[best])) best = i;
            return best;
        };
        const QByteArray low = tone(100), high = tone(5000);
        QCOMPARE(low.size(), Spectrum::BandCount);
        QVERIFY(static_cast<unsigned char>(low[loudest(low)]) > 150);
        QVERIFY(loudest(low) < loudest(high)); // a higher tone lands further right
        QCOMPARE(loudest(high), Spectrum::bandOf(5000, rate));
    }

    void backendSwitchesBetweenDisplayModes()
    {
        QTemporaryDir dir;
        FakeLink *link = nullptr;
        QStringList commands;
        auto backend = makeBackend(dir, &link);
        backend->setServiceRunner([&](const QStringList &args, QString *) { commands.append(args.join(' ')); return 0; });

        backend->setOledClock(true);
        backend->setOledMusic(true);
        QVERIFY(backend->oledMusic());
        QVERIFY(!backend->oledClock()); // one thing on the display at a time
        QVERIFY(commands.contains("--user disable --now omaglow-clock.service"));
        QCOMPARE(commands.last(), QString("--user enable --now omaglow-music.service"));

        backend->setMusicStyle(2);
        QCOMPARE(backend->musicStyle(), 2);
        QCOMPARE(commands.last(), QString("--user restart omaglow-music.service")); // picks up the style

        backend->chooseOledPreset(1);
        QVERIFY(!backend->oledMusic());
        QCOMPARE(commands.last(), QString("--user disable --now omaglow-music.service"));

        // Both enabled outside the app: music stays, the clock is turned off.
        backend->setServiceRunner([&](const QStringList &args, QString *) {
            commands.append(args.join(' '));
            return args.contains("is-enabled") ? 0 : 0;
        });
        backend->syncClockService();
        QVERIFY(backend->oledMusic());
        QVERIFY(!backend->oledClock());
        QCOMPARE(commands.last(), QString("--user disable --now omaglow-clock.service"));
    }

    void musicServiceResendsTheHeaderAfterSleep()
    {
        FakeLink *link = nullptr;
        bool present = false;
        int finds = 0;
        MusicService music(2,
            [&] { ++finds; return present ? DeviceNode{DeviceNode::Found, "/dev/null", "1ACE", 2} : DeviceNode{}; },
            [&](const QString &) -> Link * { link = new FakeLink; return link; });
        music.feed(QList<float>(Spectrum::WindowSize, 0.25f));

        // No keyboard: it looks again only every couple of seconds, not every frame.
        for (int i = 0; i < 20; ++i)
            music.tick();
        QCOMPARE(finds, 1);

        present = true;
        for (int i = 0; i < 20; ++i)
            music.tick();
        QVERIFY(link);
        QCOMPARE(link->sent.first(), Protocol::musicSpectrumStart(2));

        // Asleep behind the receiver, then awake: the header goes out again.
        link->asleep = true;
        music.tick();
        link->asleep = false;
        link->sent.clear();
        music.tick();
        QCOMPARE(link->sent.first(), Protocol::musicSpectrumStart(2));
        QVERIFY(link->sent.last().startsWith(raw("67020000")));
    }

    void profilesMatchCaptureAndKeepTheirOwnSettings()
    {
        QCOMPARE(Protocol::switchProfile(2), hex("5100000002"));
        QCOMPARE(Protocol::switchProfile(Protocol::DefaultProfile), hex("5100000006"));

        QTemporaryDir dir;
        {
            FakeLink *link = nullptr;
            auto backend = makeBackend(dir, &link);
            QCOMPARE(backend->profile(), int(Protocol::DefaultProfile));
            backend->setEffect(Protocol::Breathing);
            backend->remap(0x1e, 0x6e, 0x00);
            backend->flush();

            link->sent.clear();
            backend->setProfile(2);
            // The profile's own effect follows, or a design drawn before would stay.
            QCOMPARE(link->sent.size(), 3);
            QCOMPARE(link->sent[0], hex("5100000002"));
            QCOMPARE(link->sent[1], hex("5055"));
            QVERIFY(link->sent[2].startsWith(raw("512c00")));
            QCOMPARE(backend->profile(), 2);
            // A profile never set up from the app starts from the defaults.
            QCOMPARE(backend->effect(), int(Protocol::Static));
            QCOMPARE(backend->remappedCount(), 0);
            backend->setEffect(Protocol::Rainbow);
            backend->flush();

            backend->setProfile(Protocol::DefaultProfile);
            QCOMPARE(backend->effect(), int(Protocol::Breathing));
            QCOMPARE(backend->keyAction(0x1e), (Keymap::Action{0x6e, 0x00}));
            backend->setProfile(2);
        }
        // The chosen profile and its settings survive a restart.
        FakeLink *link = nullptr;
        auto backend = makeBackend(dir, &link);
        QCOMPARE(backend->profile(), 2);
        QCOMPARE(backend->effect(), int(Protocol::Rainbow));

        link->reject = true;
        backend->setProfile(3);
        QCOMPARE(backend->profile(), 2); // the keyboard refused, nothing changes
    }

    void backendSendsLightingAndSaves()
    {
        QTemporaryDir dir;
        FakeLink *link = nullptr;
        auto backend = makeBackend(dir, &link);
        QCOMPARE(backend->connection(), int(Backend::Connected));
        QCOMPARE(backend->firmware(), QString("4.00.18"));
        QCOMPARE(backend->battery(), 100);

        link->sent.clear();
        backend->setEffect(Protocol::Breathing);
        backend->setBrightness(50);
        backend->flush();
        QCOMPARE(link->sent.size(), 2);
        QCOMPARE(link->sent[0], hex("512c01001e3200ffffffffff"));
        QCOMPARE(link->sent[1], hex("5055"));

        link->sent.clear();
        backend->setLightingOn(false);
        backend->flush();
        QCOMPARE(link->sent.first(), Protocol::lightingOff());
    }

    // A painted design goes out in direct mode, scaled by brightness, without a save.
    void backendSendsACustomDesign()
    {
        QTemporaryDir dir;
        FakeLink *link = nullptr;
        auto backend = makeBackend(dir, &link);
        link->sent.clear();
        backend->paintKeys({0x12, 0x1f, 0x20, 0x21}, QColor("#ff0000"));
        QVERIFY(backend->customLighting());
        QCOMPARE(backend->design().value("18").value<QColor>(), QColor("#ff0000"));
        backend->setBrightness(50);
        backend->flush();
        // Like Armoury: the whole 16x6 matrix, row by row, 15 LEDs per report.
        QCOMPARE(link->sent.size(), 7);
        QCOMPARE(link->posted, 7); // written without waiting for answers
        QVERIFY(link->sent[0].startsWith(raw("c0816000 00")));
        QCOMPARE(static_cast<unsigned char>(link->sent[0][8]), 0x08); // F1 next to Esc
        QVERIFY(link->sent[6].startsWith(raw("c0810600 55")));
        for (const QByteArray &packet : link->sent)
            QVERIFY(packet.startsWith(raw("c081")));
        QByteArray all;
        for (const QByteArray &packet : link->sent)
            all += packet.mid(4, qMin(15, int(static_cast<unsigned char>(packet[2]))) * 4);
        QCOMPARE(all.size(), 96 * 4);
        QVERIFY(all.contains(raw("127f0000"))); // W at half brightness
        QVERIFY(all.contains(raw("0b7f0000"))); // A
        QVERIFY(all.contains(raw("35000000"))); // Space, not painted, stays dark

        // Picking a built-in effect leaves direct mode.
        link->sent.clear();
        backend->setEffect(Protocol::Static);
        backend->flush();
        QVERIFY(!backend->customLighting());
        QVERIFY(link->sent.first().startsWith(raw("512c00")));

        link->sent.clear();
        backend->setCustomLighting(true);
        backend->setLightingOn(false);
        backend->flush();
        QCOMPARE(link->sent.first(), Protocol::lightingOff());
    }

    void backendFillsRandomAndClearsTheDesign()
    {
        QTemporaryDir dir;
        FakeLink *link = nullptr;
        auto backend = makeBackend(dir, &link);
        backend->randomDesign();
        QCOMPARE(backend->design().size(), Keymap::layout().size());
        QSet<QString> colors;
        for (const QVariant &c : backend->design())
            colors.insert(c.value<QColor>().name());
        QVERIFY(colors.size() > 1);
        backend->fillDesign(QColor("#00ff00"));
        QCOMPARE(backend->design().value("61").value<QColor>(), QColor("#00ff00"));
        QCOMPARE(backend->design().size(), Keymap::layout().size());
        backend->clearDesign();
        QVERIFY(backend->design().isEmpty());
    }

    // Each profile keeps its own design, and it is sent again on connect.
    void customDesignIsPerProfileAndResentOnConnect()
    {
        QTemporaryDir dir;
        {
            FakeLink *link = nullptr;
            auto backend = makeBackend(dir, &link);
            backend->paintKeys({0x12}, QColor("#ff0000"));
            backend->flush();
            backend->setProfile(2);
            QVERIFY(!backend->customLighting());
            QVERIFY(backend->design().isEmpty());
            link->sent.clear();
            backend->setProfile(Protocol::DefaultProfile);
            QVERIFY(backend->customLighting());
            // The keyboard draws its own effect after a switch; the design goes back on.
            QVERIFY(link->sent.size() > 2);
            QVERIFY(link->sent[2].startsWith(raw("c081")));
        }
        FakeLink *link = nullptr;
        auto backend = makeBackend(dir, &link);
        QVERIFY(backend->customLighting());
        QCOMPARE(backend->design().size(), 1);
        bool resent = false;
        for (const QByteArray &packet : link->sent)
            resent = resent || packet.startsWith(raw("c081"));
        QVERIFY(resent);
    }

    void backendHandsAnimatedLayersToTheService()
    {
        QTemporaryDir dir;
        FakeLink *link = nullptr;
        QStringList commands;
        {
            auto backend = makeBackend(dir, &link);
            backend->setServiceRunner([&](const QStringList &args, QString *) { commands.append(args.join(' ')); return 0; });
            QTRY_VERIFY(link);
            QCOMPARE(backend->addLayer("Glow"), 0);
            backend->setLayerKeys(0, {0x12, 0x1f}, true);
            backend->setLayerKeys(0, {0x1f}, false);
            backend->setLayerColor(0, 0, QColor("#00ff00"));
            backend->flush();
            QVERIFY(backend->customLighting());
            QVERIFY(!backend->animatedLighting());
            QCOMPARE(backend->frameAt(0).value("18").value<QColor>(), QColor("#00ff00"));

            // Moving lighting is streamed by the service, not the app.
            link->sent.clear();
            backend->setLayerEffect(0, Design::Breathing);
            backend->flush();
            QVERIFY(backend->animatedLighting());
            QCOMPARE(commands.last(), QString("--user enable --now omaglow-lights.service"));
            QVERIFY(link->sent.isEmpty());
            QVERIFY(backend->frameAt(0) != backend->frameAt(1500));

            // Back to still: the app draws it and the service isn't needed.
            backend->setLayerEffect(0, Design::StaticLayer);
            backend->flush();
            QCOMPARE(commands.last(), QString("--user disable --now omaglow-lights.service"));
            QVERIFY(!link->sent.isEmpty() && link->sent.last().startsWith(raw("c081")));

            QCOMPARE(backend->addLayer("Top"), 1);
            backend->setLayerSpeed(1, 2);
            backend->setLayerRandom(1, true);
            backend->renameLayer(1, " Stars ");
            backend->moveLayer(1, 0);
            QCOMPARE(backend->layers()[0].toMap().value("name").toString(), QString("Stars"));
            backend->removeLayer(5);
            QCOMPARE(backend->layers().size(), 2);
            backend->saveLook("Glow look");
        }
        // Layers belong to the profile and to the look.
        auto backend = makeBackend(dir, &link);
        QCOMPARE(backend->layers().size(), 2);
        const QVariantMap stars = backend->layers()[0].toMap();
        QCOMPARE(stars.value("speed").toInt(), 2);
        QVERIFY(stars.value("randomColors").toBool());
        QCOMPARE(backend->layers()[1].toMap().value("keys").toList(), QVariantList{0x12});
        backend->removeLayer(0);
        backend->applyLook(0);
        QCOMPARE(backend->layers().size(), 2);
    }

    void stillLightingWaitsForTheStreamToStop()
    {
        // The service streams in its own process; still lighting sent before
        // it stops would be covered by its next frame.
        QTemporaryDir dir;
        FakeLink *link = nullptr;
        auto backend = makeBackend(dir, &link);
        QTRY_VERIFY(link);
        QList<QByteArray> events;
        link->log = &events;
        backend->setServiceRunner([&](const QStringList &args, QString *) {
            events.append(args.join(' ').toUtf8());
            return 0;
        });
        backend->setProfile(1);
        backend->addLayer("Glow");
        backend->setLayerKeys(0, {0x12}, true);
        backend->setLayerEffect(0, Design::Breathing);
        backend->flush();
        QCOMPARE(events.last(), QByteArray("--user enable --now omaglow-lights.service"));

        auto stopsFirst = [&] {
            const int stop = events.indexOf("--user disable --now omaglow-lights.service");
            QVERIFY(stop >= 0);
            for (int i = 0; i < stop; ++i)
                QVERIFY(!events[i].startsWith(raw("512c")) && !events[i].startsWith(raw("c081")));
        };
        // Switching to a profile with a normal effect.
        events.clear();
        backend->setProfile(2);
        stopsFirst();
        QVERIFY(events.last().startsWith(raw("512c")));

        // Back to the moving one, then picking a normal effect there.
        backend->setProfile(1);
        events.clear();
        backend->setEffect(Protocol::Static);
        backend->flush();
        stopsFirst();
    }

    void lightServiceStreamsMovingLayers()
    {
        QTemporaryDir dir;
        auto *settings = new QSettings(dir.path() + "/settings.ini", QSettings::IniFormat);
        settings->setValue("device/profile", 1);
        Design::ProfileLighting profile;
        profile.custom = true;
        Design::Layer layer;
        layer.effect = Design::Breathing;
        layer.keys = {0x12};
        profile.layers = {layer};
        Design::storeAt(*settings, "profile1/", profile);
        settings->sync();
        QList<QByteArray> sent;
        LightService service([] { return DeviceNode{DeviceNode::Found, "/dev/null", "1A83"}; },
                             [&](const QString &) -> Link * { auto *l = new FakeLink; l->log = &sent; return l; },
                             settings);
        settings->setParent(&service);

        // As Armoury does: color cycle first (not saved), or the keyboard
        // only draws the first frame.
        service.tick();
        QCOMPARE(sent.size(), 8);
        QCOMPARE(sent[0], Protocol::directStart());
        QVERIFY(sent[0].startsWith(raw("512c02001f6400ffff")));
        QVERIFY(sent[1].startsWith(raw("c0816000")));
        QTRY_VERIFY(sent.size() >= 22); // frames keep coming
        for (const QByteArray &packet : sent.mid(1, 21))
            QVERIFY(packet.startsWith(raw("c081")));
        // Other commands (the app's battery check) put the keyboard back to
        // drawing one frame, so the step is repeated while streaming.
        sent.clear();
        QTRY_VERIFY_WITH_TIMEOUT(sent.contains(Protocol::directStart()), 2000);

        // Still lighting is the app's to draw: the stream stops.
        profile.layers[0].effect = Design::StaticLayer;
        Design::storeAt(*settings, "profile1/", profile);
        settings->sync();
        service.tick();
        sent.clear();
        QTest::qWait(3 * Design::FrameMs);
        QVERIFY(sent.isEmpty());
    }

    // From a capture commented in OpenRazer's razerchromacommon.c:
    // "00 3f 0000 00 09 0f 02 010501000001ff0000" sets LED 5 to static red.
    void razerReportsMatchOpenRazer()
    {
        const QByteArray r = Razer::report(0x0f, 0x02, raw("010501000001ff0000"));
        QCOMPARE(r.size(), 90);
        QCOMPARE(r.left(17), raw("003f0000 00 09 0f 02 010501000001ff0000"));
        QVERIFY(r.mid(17, 71).count('\0') == 71);
        QCOMPARE(quint8(r[88]), quint8(0xff)); // XOR of bytes 2..87
        QCOMPARE(quint8(r[89]), quint8(0x00));

        using namespace Razer;
        QCOMPARE(effect(Logo, {Static, QColor("#ff0000")}).left(17), raw("003f0000 00 09 0f 02 010401000001ff0000"));
        QCOMPARE(quint8(effect(Logo, {Static, QColor("#ff0000")})[88]), quint8(0xfe));
        QCOMPARE(effect(ScrollWheel, {Off}).left(14), raw("003f0000 00 06 0f 02 010100000000"));
        QCOMPARE(effect(Logo, {Razer::Spectrum}).left(14), raw("003f0000 00 06 0f 02 010403000000"));
        QCOMPARE(effect(Logo, {Breathing, QColor("#00ff00")}).left(17), raw("003f0000 00 09 0f 02 01040201000100ff00"));
        QCOMPARE(effect(Logo, {Breathing, QColor("#00ff00"), QColor("#0000ff"), true}).left(20),
                 raw("003f0000 00 0c 0f 02 010402020002 00ff00 0000ff"));
        // 0-100 in the app is 0-255 on the mouse.
        QCOMPARE(brightness(Logo, 100).left(11), raw("003f0000 00 03 0f 04 0104ff"));
        QCOMPARE(brightness(ScrollWheel, 0).left(11), raw("003f0000 00 03 0f 04 010100"));
    }

    void razerMouseAppliesEachZone()
    {
        FakeFeaturePort port;
        Razer::Mouse mouse(&port);
        Razer::Zone logo{Razer::Static, QColor("#ff0000")};
        logo.brightness = 50;
        QVERIFY(mouse.apply(Razer::Logo, logo));
        QCOMPARE(port.sent.size(), 2); // the effect, then its brightness
        QCOMPARE(port.sent[0], Razer::effect(Razer::Logo, logo));
        QCOMPARE(port.sent[1], Razer::brightness(Razer::Logo, 50));

        // A status other than successful or busy is a failure.
        port.status = 0x03;
        QVERIFY(!mouse.apply(Razer::ScrollWheel, {Razer::Spectrum}));
        port.status = 0x01;
        QVERIFY(mouse.apply(Razer::ScrollWheel, {Razer::Spectrum}));
    }

    void looksCarryTheMouse()
    {
        QTemporaryDir dir;
        QSettings s(dir.path() + "/looks.ini", QSettings::IniFormat);
        Design::Look horde{"Horde", {}};
        Razer::Zone logo{Razer::Breathing, QColor("#ff0000"), QColor("#ff6a00"), true};
        logo.brightness = 80;
        horde.mouse.insert(Razer::Logo, logo);
        horde.mouse.insert(Razer::ScrollWheel, Razer::Zone{Razer::Static, QColor("#ff6a00")});
        Design::storeLooks(s, {horde, Design::Look{"Keyboard only", {}}});
        const auto looks = Design::loadLooks(s);
        QCOMPARE(looks.size(), 2);
        const Razer::Zone back = looks[0].mouse.value(Razer::Logo);
        QCOMPARE(back.effect, int(Razer::Breathing));
        QCOMPARE(back.color1, QColor("#ff0000"));
        QCOMPARE(back.color2, QColor("#ff6a00"));
        QVERIFY(back.twoColors);
        QCOMPARE(back.brightness, 80);
        QCOMPARE(looks[0].mouse.value(Razer::ScrollWheel).effect, int(Razer::Static));
        // Looks saved before the mouse leave it as it is.
        QVERIFY(looks[1].mouse.isEmpty());
    }

    void applyingALookPaintsTheMouse()
    {
        QTemporaryDir dir;
        FakeLink *link = nullptr;
        auto backend = makeBackend(dir, &link);
        backend->setServiceRunner([](const QStringList &, QString *) { return 0; });
        FakeFeaturePort port;
        bool plugged = true;
        backend->setMouseOpener([&]() -> Razer::FeaturePort * {
            return plugged ? new ForwardingPort(&port) : nullptr;
        });
        backend->setMouseZone(Razer::Logo, Razer::Static, QColor("#0050ff"), QColor("#0050ff"), false, 100);
        QCOMPARE(port.sent.size(), 2);
        QCOMPARE(port.sent[0], Razer::effect(Razer::Logo, {Razer::Static, QColor("#0050ff")}));
        backend->saveLook("Alliance");
        QCOMPARE(backend->mouseZones().value("4").toMap().value("effect").toInt(), int(Razer::Static));

        backend->setMouseZone(Razer::Logo, Razer::Off, QColor("#000000"), QColor("#000000"), false, 100);
        port.sent.clear();
        backend->applyLook(0);
        QCOMPARE(port.sent.size(), 2); // only the zone the look has
        QCOMPARE(port.sent[0], Razer::effect(Razer::Logo, {Razer::Static, QColor("#0050ff")}));

        // No mouse plugged in: the keyboard part still applies.
        plugged = false;
        port.sent.clear();
        backend->applyLook(0);
        QVERIFY(port.sent.isEmpty());
    }

    void devicesPageKnowsWhetherTheMouseIsThere()
    {
        QTemporaryDir dir;
        FakeLink *link = nullptr;
        auto backend = makeBackend(dir, &link);
        DeviceNode node{DeviceNode::NotFound, {}, {}};
        backend->setMouseFinder([&] { return node; });
        QSignalSpy spy(backend.get(), &Backend::mouseChanged);
        backend->refreshMouse();
        QCOMPARE(backend->mouseStatus(), int(DeviceNode::NotFound));
        node = DeviceNode{DeviceNode::NoPermission, "/dev/hidraw6", "0084"};
        backend->refreshMouse();
        QCOMPARE(backend->mouseStatus(), int(DeviceNode::NoPermission));
        node.status = DeviceNode::Found;
        backend->refreshMouse();
        QCOMPARE(backend->mouseStatus(), int(DeviceNode::Found));
        QCOMPARE(spy.count(), 2); // only changes are announced
        backend->refreshMouse();
        QCOMPARE(spy.count(), 2);
    }

    // As `asusctl aura effect --help` and `asusctl leds --help` (asusctl 6.5) describe them.
    void laptopAuraBecomesAsusctlCommands()
    {
        using namespace Laptop;
        Aura a;
        a.effect = Laptop::Static;
        a.color1 = QColor("#ff0000");
        a.brightness = 3;
        QCOMPARE(commands(a), (QList<QStringList>{{"aura", "effect", "static", "-c", "ff0000"}, {"leds", "set", "high"}}));
        a.effect = Breathe;
        a.color2 = QColor("#ffaa00");
        a.speed = 0;
        a.brightness = 1;
        QCOMPARE(commands(a), (QList<QStringList>{
            {"aura", "effect", "breathe", "--colour", "ff0000", "--colour2", "ffaa00", "--speed", "low"},
            {"leds", "set", "low"}}));
        a.effect = RainbowCycle;
        a.speed = 2;
        QCOMPARE(commands(a).first(), (QStringList{"aura", "effect", "rainbow-cycle", "--speed", "high"}));
        a.effect = RainbowWave;
        a.direction = 2;
        QCOMPARE(commands(a).first(), (QStringList{"aura", "effect", "rainbow-wave", "--direction", "left", "--speed", "high"}));
        // Off is only the brightness: the effect stays for when it comes back on.
        a.brightness = 0;
        QCOMPARE(commands(a), (QList<QStringList>{{"leds", "set", "off"}}));

        bool ok = false;
        const Aura back = decode(encode(a), &ok);
        QVERIFY(ok);
        QCOMPARE(encode(back), encode(a));
        decode("nonsense", &ok);
        QVERIFY(!ok);
    }

    void looksCarryTheLaptopKeyboard()
    {
        QTemporaryDir dir;
        FakeLink *link = nullptr;
        auto backend = makeBackend(dir, &link);
        backend->setServiceRunner([](const QStringList &, QString *) { return 0; });
        QList<QStringList> ran;
        backend->setAsusctlRunner([&](const QStringList &args) { ran.append(args); return 0; });
        backend->setLaptopAura(Laptop::Breathe, QColor("#0050ff"), QColor("#ffb000"), 1, 3, 3);
        QCOMPARE(ran.size(), 2);
        QCOMPARE(ran[0].mid(0, 3), (QStringList{"aura", "effect", "breathe"}));
        QCOMPARE(backend->laptopAura().value("effect").toInt(), int(Laptop::Breathe));
        backend->saveLook("Alliance");

        backend->setLaptopAura(Laptop::Static, QColor("#ffffff"), QColor("#ffffff"), 1, 3, 3);
        ran.clear();
        backend->applyLook(0);
        QCOMPARE(ran.size(), 2);
        QCOMPARE(ran[0], (QStringList{"aura", "effect", "breathe", "--colour", "0050ff", "--colour2", "ffb000", "--speed", "med"}));

        // Kept across restarts, and looks without it leave the laptop alone.
        auto again = makeBackend(dir, &link);
        QCOMPARE(again->laptopAura().value("effect").toInt(), int(Laptop::Breathe));
        QTemporaryDir other;
        QSettings s(other.path() + "/l.ini", QSettings::IniFormat);
        Design::storeLooks(s, {Design::Look{"Keyboard only", {}}});
        QVERIFY(!Design::loadLooks(s).first().hasLaptop);
    }

    // Omarchy renders keyboard.rgb from {{ accent }} unless the theme ships its
    // own (Tokyo Night ships ff00ff).
    void themeKeyboardColorComesFromKeyboardRgb()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        QFile colors(dir.path() + "/theme/colors.toml");
        colors.open(QIODevice::WriteOnly);
        colors.write("accent = \"#7aa2f7\"\n");
        colors.close();
        QCOMPARE(Theme::readKeyboardColor(dir.path()), QColor("#7aa2f7"));
        QFile rgb(dir.path() + "/theme/keyboard.rgb");
        rgb.open(QIODevice::WriteOnly);
        rgb.write("ff00ff\n");
        rgb.close();
        QCOMPARE(Theme::readKeyboardColor(dir.path()), QColor("#ff00ff"));
    }

    void followingTheThemePaintsEveryDevice()
    {
        QTemporaryDir dir, hooks;
        FakeLink *link = nullptr;
        auto backend = makeBackend(dir, &link);
        backend->setServiceRunner([](const QStringList &, QString *) { return 0; });
        QList<QStringList> ran;
        backend->setAsusctlRunner([&](const QStringList &args) { ran.append(args); return 0; });
        FakeFeaturePort port;
        backend->setMouseOpener([&]() -> Razer::FeaturePort * { return new ForwardingPort(&port); });
        backend->setHooksDirectory(hooks.path());
        QFile legacy(hooks.path() + "/50-azoth");
        legacy.open(QIODevice::WriteOnly);
        legacy.write("azoth theme-sync\n");
        legacy.close();

        // On: Omarchy's theme-set hook runs azoth when the app is closed.
        backend->setFollowTheme(true);
        QFile hook(hooks.path() + "/50-omaglow");
        // The hook azoth installed is replaced by Omaglow's.
        QVERIFY(!QFile::exists(hooks.path() + "/50-azoth"));
        QVERIFY(hook.exists());
        QVERIFY(hook.permissions() & QFileDevice::ExeUser);
        hook.open(QIODevice::ReadOnly);
        QVERIFY(hook.readAll().contains("omaglow theme-sync"));

        link->sent.clear();
        backend->applyThemeColor(QColor("#7aa2f7"));
        backend->flush();
        QVERIFY(!backend->customLighting());
        QCOMPARE(backend->effect(), int(Protocol::Static));
        QCOMPARE(backend->color1(), QColor("#7aa2f7"));
        QVERIFY(!link->sent.isEmpty());
        QCOMPARE(port.sent.size(), 4); // logo and wheel: effect and brightness each
        QCOMPARE(port.sent[0], Razer::effect(Razer::Logo, {Razer::Static, QColor("#7aa2f7")}));
        QCOMPARE(ran.first(), (QStringList{"aura", "effect", "static", "-c", "7aa2f7"}));

        // The same color again changes nothing (the keyboard stores each effect).
        link->sent.clear();
        port.sent.clear();
        ran.clear();
        backend->applyThemeColor(QColor("#7aa2f7"));
        backend->flush();
        QVERIFY(link->sent.isEmpty());
        QVERIFY(port.sent.isEmpty());
        QVERIFY(ran.isEmpty());

        backend->setFollowTheme(false);
        QVERIFY(!QFile::exists(hooks.path() + "/50-omaglow"));
    }

    // azoth was renamed Omaglow: the old settings come along once.
    void settingsMoveOverFromAzoth()
    {
        QTemporaryDir config;
        QDir(config.path()).mkpath("azoth");
        QFile old(config.path() + "/azoth/azoth.conf");
        old.open(QIODevice::WriteOnly);
        old.write("[device]\nprofile=3\n");
        old.close();
        QVERIFY(AppInfo::migrateLegacySettings(config.path()));
        QSettings moved(config.path() + "/omaglow/omaglow.conf", QSettings::IniFormat);
        QCOMPARE(moved.value("device/profile").toInt(), 3);
        QVERIFY(QFile::exists(config.path() + "/azoth/azoth.conf")); // kept as it was
        // Once there, the new settings are never replaced.
        QSettings(config.path() + "/omaglow/omaglow.conf", QSettings::IniFormat).setValue("device/profile", 5);
        QVERIFY(!AppInfo::migrateLegacySettings(config.path()));
        QCOMPARE(QSettings(config.path() + "/omaglow/omaglow.conf", QSettings::IniFormat).value("device/profile").toInt(), 5);
        QTemporaryDir fresh;
        QVERIFY(!AppInfo::migrateLegacySettings(fresh.path()));
        QCOMPARE(QString(AppInfo::Name), QString("omaglow"));
    }

    void razerMouseIsFoundOnItsFirstInterface()
    {
        QTemporaryDir sys, dev;
        const auto node = [&](const QString &name, const QString &id, const QString &phys) {
            QDir(sys.path()).mkpath(name + "/device");
            QFile uevent(sys.path() + "/" + name + "/device/uevent");
            uevent.open(QIODevice::WriteOnly);
            uevent.write(QString("DRIVER=hid-generic\nHID_ID=%1\nHID_NAME=Razer\nHID_PHYS=%2\n").arg(id, phys).toUtf8());
            QFile(dev.path() + "/" + name).open(QIODevice::WriteOnly);
        };
        node("hidraw3", "0003:00000B05:00001A83", "usb-0000:00:14.0-2.1/input0");
        node("hidraw10", "0003:00001532:00000084", "usb-0000:00:14.0-2.2.3/input1");
        node("hidraw6", "0003:00001532:00000084", "usb-0000:00:14.0-2.2.3/input0");
        const DeviceNode found = Razer::find(sys.path(), dev.path());
        QCOMPARE(found.status, DeviceNode::Found);
        QCOMPARE(found.path, dev.path() + "/hidraw6");
        QCOMPARE(found.productId, QString("0084"));

        QTemporaryDir empty;
        QCOMPARE(Razer::find(empty.path(), dev.path()).status, DeviceNode::NotFound);
    }

    void appAndServicesTakeTurnsWithTheKeyboard()
    {
        // The app and the light service talking at once left the keyboard
        // stuck until replugged; every frame and command holds a shared lock.
        const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
        QCOMPARE(DeviceLock::path(), runtime + "/omaglow-keyboard.lock");
        {
            DeviceLock outer;
            DeviceLock inner; // nested in one process: no deadlock
        }

        int unlocked = 0;
        QTemporaryDir dir;
        auto *settings = new QSettings(dir.path() + "/settings.ini", QSettings::IniFormat);
        settings->setValue("device/profile", 1);
        Design::ProfileLighting profile;
        profile.custom = true;
        Design::Layer layer;
        layer.effect = Design::Breathing;
        layer.keys = {0x12};
        profile.layers = {layer};
        Design::storeAt(*settings, "profile1/", profile);
        settings->sync();
        QList<QByteArray> sent;
        LightService service([] { return DeviceNode{DeviceNode::Found, "/dev/null", "1A83"}; },
                             [&](const QString &) -> Link * {
                                 auto *l = new FakeLink;
                                 l->log = &sent;
                                 l->checkLock = true;
                                 l->unlocked = &unlocked;
                                 return l;
                             },
                             settings);
        settings->setParent(&service);
        service.tick();
        QTRY_VERIFY(sent.size() >= 16);
        QCOMPARE(unlocked, 0);

        QTemporaryDir appDir;
        auto *appSettings = new QSettings(appDir.path() + "/settings.ini", QSettings::IniFormat);
        auto backend = std::make_unique<Backend>(
            [] { return DeviceNode{DeviceNode::Found, "/dev/null", "1A83"}; },
            [&](const QString &) -> Link * {
                auto *l = new FakeLink;
                l->checkLock = true;
                l->unlocked = &unlocked;
                return l;
            },
            appSettings, nullptr);
        appSettings->setParent(backend.get());
        backend->setServiceRunner([](const QStringList &, QString *) { return 0; });
        backend->setEffect(Protocol::Rainbow);
        backend->flush();
        backend->remap(0x1e, 0x6e, 0x00);
        QCOMPARE(unlocked, 0);
    }

    void keyboardThatDoesntAnswerIsntConnected()
    {
        QTemporaryDir dir;
        bool mute = true;
        int opened = 0;
        FakeLink *last = nullptr;
        auto *settings = new QSettings(dir.path() + "/settings.ini", QSettings::IniFormat);
        auto backend = std::make_unique<Backend>(
            [] { return DeviceNode{DeviceNode::Found, "/dev/null", "1A83"}; },
            [&](const QString &) -> Link * { ++opened; last = new FakeLink; last->mute = mute; return last; },
            settings, nullptr);
        settings->setParent(backend.get());
        // On the cable the keyboard always answers; silence means it is stuck.
        QCOMPARE(backend->connection(), int(Backend::NotResponding));
        QCOMPARE(opened, 1);

        // Every check would freeze the window while it waits, so a stuck keyboard
        // is asked once, briefly, every third check (6 s).
        backend->poll();
        backend->poll();
        QCOMPARE(opened, 1);
        backend->poll();
        QCOMPARE(opened, 2);
        QCOMPARE(last->sent.size(), 1);
        QCOMPARE(last->timeoutMs, 150);

        // Unplugged and plugged back in: found again, waiting as long as usual.
        mute = false;
        for (int i = 0; i < 3; ++i)
            backend->poll();
        QCOMPARE(backend->connection(), int(Backend::Connected));
        QVERIFY(!backend->firmware().isEmpty());
        QCOMPARE(last->timeoutMs, 400);
    }

    void keyboardThatStopsAnsweringIsNoticed()
    {
        QTemporaryDir dir;
        FakeLink *link = nullptr;
        auto backend = makeBackend(dir, &link);
        QCOMPARE(backend->connection(), int(Backend::Connected));
        // Writes still go through but nothing comes back: by the next battery check.
        link->mute = true;
        for (int i = 0; i < 15; ++i)
            backend->poll();
        QCOMPARE(backend->connection(), int(Backend::NotResponding));
    }

    void oneLostBatteryReplyIsNotAStuckKeyboard()
    {
        QTemporaryDir dir;
        FakeLink *link = nullptr;
        auto backend = makeBackend(dir, &link);
        // A reply can come late while the light stream is busy: ask again first.
        link->dropNext = 1;
        for (int i = 0; i < 15; ++i)
            backend->poll();
        QCOMPARE(backend->connection(), int(Backend::Connected));
    }

    void looksShowWhichOneIsOn()
    {
        QTemporaryDir dir;
        FakeLink *link = nullptr;
        auto backend = makeBackend(dir, &link);
        backend->setServiceRunner([](const QStringList &, QString *) { return 0; });
        QTRY_VERIFY(link);
        const auto active = [&](int i) { return backend->looks()[i].toMap().value("active").toBool(); };
        backend->setEffect(Protocol::Rainbow);
        backend->saveLook("Rainbow");
        QVERIFY(active(0));
        backend->paintKeys({0x12}, QColor("#ff0000"));
        backend->saveLook("Red W");
        QVERIFY(!active(0));
        QVERIFY(active(1));
        QSignalSpy spy(backend.get(), &Backend::looksChanged);
        backend->applyLook(0);
        QVERIFY(spy.count() > 0);
        QVERIFY(active(0));
        QVERIFY(!active(1));
        backend->setSpeed(2); // edited: no longer the saved look
        QVERIFY(!active(0));

        // Colors read back from the settings file are the same colors.
        backend->paintKeys({0x13}, QColor::fromHsvF(0.33, 0.7, 0.9));
        backend->saveLook("Hsv");
        backend->setProfile(1);
        backend->setProfile(Protocol::DefaultProfile);
        QVERIFY(active(2));
    }

    void newLayersStartWithOneColor()
    {
        QTemporaryDir dir;
        FakeLink *link = nullptr;
        auto backend = makeBackend(dir, &link);
        backend->setServiceRunner([](const QStringList &, QString *) { return 0; });
        const int index = backend->addLayer("");
        const QVariantMap layer = backend->layers()[index].toMap();
        // Effects that alternate show only the color picked, not a stray second one.
        QCOMPARE(layer.value("color2").value<QColor>(), layer.value("color1").value<QColor>());
    }

    void lightServicePacesItsReports()
    {
        // Reports sent back to back left the keyboard stuck after minutes of
        // streaming; OpenRGB waits 20 ms after each one.
        QTemporaryDir dir;
        auto *settings = new QSettings(dir.path() + "/settings.ini", QSettings::IniFormat);
        settings->setValue("device/profile", 1);
        Design::ProfileLighting profile;
        profile.custom = true;
        Design::Layer layer;
        layer.effect = Design::Breathing;
        layer.keys = {0x12};
        profile.layers = {layer};
        Design::storeAt(*settings, "profile1/", profile);
        settings->sync();
        QList<QByteArray> sent;
        QList<qint64> times;
        LightService service([] { return DeviceNode{DeviceNode::Found, "/dev/null", "1A83"}; },
                             [&](const QString &) -> Link * {
                                 auto *l = new FakeLink;
                                 l->log = &sent;
                                 l->times = &times;
                                 return l;
                             },
                             settings);
        settings->setParent(&service);
        service.tick();
        QTRY_VERIFY(times.size() >= 16);
        for (int i = 1; i < 16; ++i)
            QVERIFY2(times[i] - times[i - 1] >= LightService::ReportGapMs - 2,
                     qPrintable(QString("reports %1 and %2 were %3 ms apart").arg(i - 1).arg(i).arg(times[i] - times[i - 1])));
    }

    void lightServiceWaitsWhileTheKeyboardSleeps()
    {
        QTemporaryDir dir;
        auto *settings = new QSettings(dir.path() + "/settings.ini", QSettings::IniFormat);
        settings->setValue("device/profile", 1);
        Design::ProfileLighting profile;
        profile.custom = true;
        Design::Layer layer;
        layer.effect = Design::Breathing;
        layer.keys = {0x12};
        profile.layers = {layer};
        Design::storeAt(*settings, "profile1/", profile);
        settings->sync();
        QList<QByteArray> sent;
        bool asleep = true; // only the receiver answers, with ff aa
        LightService service([] { return DeviceNode{DeviceNode::Found, "/dev/null", "1A85"}; },
                             [&](const QString &) -> Link * {
                                 auto *link = new FakeLink;
                                 link->log = &sent;
                                 link->asleep = asleep;
                                 return link;
                             },
                             settings);
        settings->setParent(&service);

        // A frame the receiver turns away isn't a frame drawn: stop until the next check.
        service.tick();
        const int tried = sent.size();
        QTest::qWait(4 * Design::FrameMs);
        QCOMPARE(sent.size(), tried);

        // Awake again: the next check streams.
        asleep = false;
        service.tick();
        QTRY_VERIFY(sent.size() >= tried + 15);
    }

    void lightsServiceIsBroughtBackOnLaunch()
    {
        QTemporaryDir dir;
        FakeLink *link = nullptr;
        {
            auto backend = makeBackend(dir, &link);
            backend->setServiceRunner([](const QStringList &, QString *) { return 0; });
            QTRY_VERIFY(link);
            backend->addLayer("Glow");
            backend->setLayerKeys(0, {0x12}, true);
            backend->setLayerEffect(0, Design::Breathing);
            backend->flush();
        }
        // Stopped behind the app's back (a crash, systemctl stop): launching
        // starts it again, since nothing else draws the moving layers.
        auto backend = makeBackend(dir, &link);
        QStringList commands;
        backend->setServiceRunner([&](const QStringList &args, QString *) {
            commands.append(args.join(' '));
            return args.contains("is-active") ? 3 : 0;
        });
        backend->syncLightsService();
        QCOMPARE(commands.last(), QString("--user enable --now omaglow-lights.service"));

        // Running and wanted: left alone.
        commands.clear();
        backend->setServiceRunner([&](const QStringList &args, QString *) { commands.append(args.join(' ')); return 0; });
        backend->syncLightsService();
        QCOMPARE(commands, QStringList{"--user is-active --quiet omaglow-lights.service"});
    }

    void backendRemembersSettings()
    {
        QTemporaryDir dir;
        {
            FakeLink *link = nullptr;
            auto backend = makeBackend(dir, &link);
            backend->setEffect(Protocol::Rainbow);
            backend->setDirection(4);
            backend->flush();
        }
        FakeLink *link = nullptr;
        auto backend = makeBackend(dir, &link);
        QCOMPARE(backend->effect(), int(Protocol::Rainbow));
        QCOMPARE(backend->direction(), 4);
    }

    void backendReportsRejection()
    {
        QTemporaryDir dir;
        FakeLink *link = nullptr;
        auto backend = makeBackend(dir, &link);
        link->reject = true;
        backend->setOledBrightness(40);
        QVERIFY(!backend->error().isEmpty());
        QCOMPARE(backend->oledBrightness(), 100);
    }

    void backendReportsMissingKeyboardAndPermission()
    {
        QTemporaryDir dir;
        QSettings settings(dir.path() + "/s.ini", QSettings::IniFormat);
        FakePicker picker;
        Backend missing([] { return DeviceNode{}; }, [](const QString &) -> Link * { return nullptr; },
                        &settings, &picker);
        QCOMPARE(missing.connection(), int(Backend::Disconnected));

        Backend denied([] { return DeviceNode{DeviceNode::NoPermission, "/dev/hidraw5", "1A83"}; },
                       [](const QString &) -> Link * { return nullptr; }, &settings, &picker);
        QCOMPARE(denied.connection(), int(Backend::NoPermission));
        QCOMPARE(denied.devicePath(), QString("/dev/hidraw5"));

        // Nothing is sent while there is no keyboard; the user is told why.
        missing.uploadOled(dir.path() + "/x.png");
        QVERIFY(!missing.error().isEmpty());
    }

    void backendUploadsAPicture()
    {
        QTemporaryDir dir;
        QImage picture(208, 64, QImage::Format_RGB32);
        picture.fill(Qt::white);
        QVERIFY(picture.save(dir.path() + "/p.png"));

        FakeLink *link = nullptr;
        auto backend = makeBackend(dir, &link);
        link->sent.clear();
        backend->uploadOled(dir.path() + "/p.png");
        QVERIFY(backend->busy());
        QTRY_VERIFY(!backend->busy());
        QCOMPARE(backend->progress(), 1.0);
        QCOMPARE(link->sent.size(), 1 + 111 + 1 + 1);
        QCOMPARE(link->sent.first(), hex("610100006f00"));
        QCOMPARE(link->sent.last(), hex("5055"));
        QCOMPARE(backend->oledFrames(), 1);
        QCOMPARE(backend->previewFrame(0).size(), QSize(208, 64));
    }

    void pickerSelectionUploads()
    {
        QTemporaryDir dir;
        QImage picture(10, 10, QImage::Format_RGB32);
        picture.fill(Qt::white);
        QVERIFY(picture.save(dir.path() + "/p.png"));
        FakeLink *link = nullptr;
        FakePicker *picker = nullptr;
        auto backend = makeBackend(dir, &link, &picker);
        backend->openOledImage();
        QCOMPARE(picker->opened, 1);
        emit picker->openSelected(QUrl::fromLocalFile(dir.path() + "/p.png"));
        QTRY_VERIFY(!backend->busy());
        QCOMPARE(backend->oledFile(), dir.path() + "/p.png");
    }

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        QFile colors(dir.path() + "/theme/colors.toml");
        QVERIFY(colors.open(QIODevice::WriteOnly));
        colors.write("accent = \"#112233\"\n");
        colors.close();

        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void themeFollowsHyprlandRounding()
    {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        auto write = [](const QString &path, const QByteArray &data) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
        };
        QVERIFY(write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n"));
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(write(response, "{\"int\":0}"));

        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        QVERIFY(write(response, "{\"int\":6}"));
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "pt-BR\n");
        write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})");
        write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    // Every string the app shows has a Spanish translation, so a new one
    // can't ship in English by mistake.
    void spanishCatalogCoversEveryString()
    {
        QFile catalogFile(QStringLiteral(OMAGLOW_SOURCE_DIR "/i18n/es.json"));
        QVERIFY(catalogFile.open(QIODevice::ReadOnly));
        const QJsonObject catalog = QJsonDocument::fromJson(catalogFile.readAll()).object();
        QVERIFY(!catalog.isEmpty());

        QStringList sources;
        static const QRegularExpression calls(
            "(?:\\bqsTr|(?<![A-Za-z_])tr|QT_TR_NOOP)\\(\\s*\"((?:[^\"\\\\]|\\\\.)*)\"|"
            "(?:QT_TRANSLATE_NOOP|translate)\\(\\s*\"[^\"]*\"\\s*,\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");
        const QDir dir(QStringLiteral(OMAGLOW_SOURCE_DIR));
        for (const QString &name : dir.entryList({"*.qml", "*.cpp"}, QDir::Files)) {
            if (name == "translator.cpp")
                continue;
            QFile file(dir.filePath(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QString text = QString::fromUtf8(file.readAll());
            for (auto it = calls.globalMatch(text); it.hasNext();) {
                const auto m = it.next();
                const QString literal = m.captured(1).isNull() ? m.captured(2) : m.captured(1);
                // Back to the runtime text: the catalog keys are unescaped.
                sources << QJsonDocument::fromJson(("[\"" + literal + "\"]").toUtf8()).array().at(0).toString();
            }
        }
        // Names kept in tables and translated where they are shown.
        for (int e = 0; e < Protocol::EffectCount; ++e)
            sources << QString::fromUtf8(Protocol::effectInfo(e).name);
        for (const Keymap::Choice &c : Keymap::specialChoices())
            sources << QString::fromUtf8(c.label);

        QVERIFY(sources.size() > 150);
        QStringList missing;
        for (const QString &s : std::as_const(sources))
            if (!s.isEmpty() && !catalog.contains(s))
                missing << s;
        missing.removeDuplicates();
        QVERIFY2(missing.isEmpty(), qPrintable("Not in es.json: " + missing.join(" | ")));
    }

    void spanishFollowsOmarchysMexicanSpanish()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "es-MX\n");
        Translator translator(dir.path() + "/language", QStringLiteral(OMAGLOW_SOURCE_DIR "/i18n"));
        QCOMPARE(translator.language(), QString("es-MX"));
        QCOMPARE(translator.translate("Main", "Lighting"), QString("Iluminación"));
        QCOMPARE(translator.translate("Main", "%n keys changed", nullptr, 1), QString("%n tecla cambiada"));
        QCOMPARE(translator.translate("Main", "%n keys changed", nullptr, 3), QString("%n teclas cambiadas"));
        QCOMPARE(translator.translate("Main", "%n keys changed", nullptr, 1000000), QString("%n teclas cambiadas"));
    }

    void translatorPicksPluralForms()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "ru\n");
        write(dir.path() + "/en.json", R"({"%n clips": {"one": "%n clip", "other": "%n clips"}})");
        write(dir.path() + "/ru.json", R"({"%n clips": {"one": "%n клип", "few": "%n клипа", "many": "%n клипов", "other": "%n клипа"}})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 21), QString("%n клип"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 22), QString("%n клипа"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 25), QString("%n клипов"));
        QCOMPARE(Translator::pluralCategory("ar", 11), QString("many"));
        QCOMPARE(Translator::pluralCategory("ja", 1), QString("other"));
    }

private:
    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }
};

QTEST_MAIN(OmaglowTests)
#include "omaglow_tests.moc"
