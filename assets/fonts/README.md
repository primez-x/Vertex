# Bundled fonts

## IBM Plex Sans and IBM Plex Mono

IBM Plex Sans is the application UI font and the sheet, PDF and report output
font; IBM Plex Mono is the numeric/monospace face named by the Vertex design
system. Both are licensed under SIL Open Font License 1.1 with Reserved Font
Name "Plex"; the full copyright and license are in `IBM-Plex-OFL.txt`
(byte-identical to the upstream `LICENSE.txt`). They are not offered under the
application's private source license.

Only the faces the design system uses are bundled, unmodified and under their
upstream file names, taken from the release archives' `fonts/complete/ttf/`
directories:

| File | Release | Font version | SHA-256 |
| --- | --- | --- | --- |
| `IBMPlexSans-Regular.ttf` | `@ibm/plex-sans@1.1.0` | 3.005 | `975dcda37d80f038dcd143c22e33ca2d97a0cc5a929aace1c749153b0fe1afa5` |
| `IBMPlexSans-Medium.ttf` | `@ibm/plex-sans@1.1.0` | 3.005 | `331c8639d7598b2cde62a911a71db195e30cb655cd6bdf2e324a7e984955f907` |
| `IBMPlexSans-SemiBold.ttf` | `@ibm/plex-sans@1.1.0` | 3.005 | `a20caf8286023a6a7a85e40b1d2a4ae9fc3e3b1f9eda8f4c542dd4986af67bb1` |
| `IBMPlexMono-Regular.ttf` | `@ibm/plex-mono@2.5.0` | 2.005 | `7c6fbddca4b700be918f5f6183d9bd4464fa427fe435f0b480d77fe2bb8c5a43` |
| `IBMPlexMono-Medium.ttf` | `@ibm/plex-mono@2.5.0` | 2.005 | `98fbd727aae340b236955879dabed4d991aac9e8e90b3b2a67ce4a59221cc97c` |
| `IBM-Plex-OFL.txt` | `@ibm/plex-sans@1.1.0` `LICENSE.txt` | | `7e6b2818edbd8f6a01ae80641cc8f16a51080d08fb4e532be3a0b6f74adb07da` |

Sources:

- <https://github.com/IBM/plex/releases/tag/%40ibm/plex-sans%401.1.0>,
  `ibm-plex-sans.zip`, SHA-256
  `fb365d910566e6d199cc2c15579a7dd9a267128e18431a394ed81f1970c69200`. The
  release publishes no checksum; the archive's WOFF/WOFF2 faces and license
  were cross-checked byte-for-byte against the signed npm package
  `@ibm/plex-sans@1.1.0`.
- <https://github.com/IBM/plex/releases/tag/%40ibm/plex-mono%402.5.0>,
  `ibm-plex-mono.zip`, SHA-256
  `6d23f01257663d8cc49a0d64c22ced630b79e0e2a0ac08a0da86e9a38bbc481c`, matching
  the release asset digest GitHub publishes. Its `LICENSE.txt` is identical to
  the Sans license.

The faces are compiled into the `sketch_desktop` `:/fonts` Qt resource and
registered at startup by `ensure_bundled_fonts()` in
`src/desktop/bundled_fonts.hpp`. Italic faces are not bundled; the UI does not
use italic text.

## Inter

The unmodified Inter 4.1 variable font remains bundled so documents,
annotation styles, text-library entries and tests that name the "Inter" family
keep rendering with the shipped face. It is licensed under SIL Open Font
License 1.1; the full copyright and license are in `OFL.txt`. It is not
offered under the application's private source license.

Source: Google Fonts `ofl/inter`, commit
`0b58fb370093f9a9f4ff785d94405710b79de67c`.
Upstream Inter source commit:
`66647c0bbbe41a850d79d9c76fb13add3378940f`.
The font's internal family names and bytes are unchanged; only the local
filename is shortened to `Inter.ttf`.

<https://github.com/google/fonts/tree/0b58fb370093f9a9f4ff785d94405710b79de67c/ofl/inter>
