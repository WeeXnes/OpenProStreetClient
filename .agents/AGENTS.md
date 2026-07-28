# ProStreetRPC Agent Rules & Guidelines

## Versioning Policy
When making code modifications to this project, you MUST update the project version string defined in `library.cpp` (`const std::string version = "X.Y";`) according to the following rules:

1. **Small Changes (+0.1)**:
   - For bug fixes, minor improvements, small feature additions, or refactorings, increment the minor version number by `0.1` (e.g., `2.0` -> `2.1`, `2.4` -> `2.5`).

2. **Big Changes (+1.0)**:
   - For major features, architectural changes, or significant overhauls, increment the major version number by `1` and reset the decimal/minor digit to `0` (e.g., `2.3` -> `3.0`, `3.7` -> `4.0`).

Always verify that the version string in `library.cpp` is updated before completing a task so that `build.sh` packages the release bundle with the appropriate version number (e.g., `ProStreetRPC_vX.Y.zip`).
