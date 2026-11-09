# Changelog

Each version's changes, newest first. The version is in `VERSION`.

## Unreleased

- A PKGBUILD for the AUR in `aur/`, building a released tag.
- `bin/protect-github` sets GitHub rulesets for `main` and the release tags.
- The README says Omaglow isn't affiliated with ASUS or Razer and never writes
  firmware, and shows the current app.

## 0.30.0

- Moving layers run smoother: each frame sends only the keys that changed, so
  streaming goes from about 7 frames a second to 9-15. The whole keyboard is
  sent again every second.
- CI on GitHub Actions: every push and pull request builds the app and runs the
  tests on Arch Linux, and checks the PKGBUILD's version.
- One version source, the `VERSION` file.

## 0.29.0

- Mark looks as favorites with a star; favorites are listed first.
- Pick Omaglow's language in the sidebar (English or Spanish), or follow
  Omarchy's as before.

## 0.28.0

- Export a look to a JSON file and import look files, from Looks. The format is
  documented in the README; importing refuses files that aren't looks and drops
  what it can't use inside one.

## 0.27.2

- Shockwave and Embers blend in linear light, so they brighten any color under
  them instead of turning it grey.

## 0.27.1

- Shockwave flashes the whole layer on each beat and its ring carries further.

## 0.27.0

- Shockwave (a drum flash and rings from the bottom row on a double beat) and
  Embers (sparks rising) layers, drawn over the layers under them.
- Start and stop a look's timer from Lighting, with the time left.
- Workspace keys follow the focused monitor's block of ten, as Omarchy numbers
  workspaces per monitor.

## 0.26.0

- The app in Spanish, following Omarchy's language.
- Studio layers: Fire, Plasma, Scanner, Fireflies, Heartbeat and Gradient.
- Live layers: Music (spectrum bars), Workspaces (Hyprland), Meter (CPU load,
  CPU temperature or battery), Notifications, Timer and Typing (ripples from
  the keys you press). Each source runs only while a layer that needs it shows.
- The keyboard isn't asked for its battery twice when unplugged, and an empty
  battery reply is asked again before calling the keyboard stuck.

## 0.25.1

- The window no longer freezes while the keyboard is stuck, and a stuck
  keyboard is noticed sooner.

## 0.25.0

- First version as Omaglow (earlier named azoth): lighting, Studio designs and
  layers, OLED, keys, knob and power for the ROG Azoth; logo and scroll wheel
  lighting for the Razer DeathAdder V2; keyboard lighting for ASUS Aura laptops
  through asusctl; looks; following the Omarchy theme.
