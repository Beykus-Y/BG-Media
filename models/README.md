# Local VRM model

`Model1.vrm` and its embedded textures are **not part of this repository**.

The VRM metadata declares:

- `allowedUserName`: `OnlyAuthor`
- `licenseName`: `Redistribution_Prohibited`
- author/title metadata applies to the original character

They are used only for local development and verification of BG Media 3D. Do
not commit the model, extracted textures, rendered frames or other derivatives
to a public repository without explicit permission from the rights holder.

The loader, retargeter and desktop controls are generic VRM 0.x code. The
repository contains a tiny, self-authored synthetic rig at
`tests/fixtures/minimal.vrm` for automated tests that does not depend on this
private model.

To run the 3D demo locally, place a VRM 0.x file at `models/Model1.vrm` (or use
`--vrm <path>` / **LOAD VRM** in the application).
