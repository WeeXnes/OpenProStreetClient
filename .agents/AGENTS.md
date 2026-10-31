# OpenProStreet Client Agent Rules & Guidelines

## Versioning Policy
When making code modifications to this project, you MUST update the project version string defined in `src/main.cpp` (`const std::string version = "X.Y";`) according to the following rules:

1. **Small Changes (+0.1)**:
   - For bug fixes, minor improvements, small feature additions, or refactorings, increment the minor version number by `0.1` (e.g., `2.0` -> `2.1`, `2.4` -> `2.5`).

2. **Big Changes (+1.0)**:
   - For major features, architectural changes, or significant overhauls, increment the major version number by `1` and reset the decimal/minor digit to `0` (e.g., `2.3` -> `3.0`, `3.7` -> `4.0`).

Always verify that the version string in `src/main.cpp` is updated before completing a task so that `build.sh` packages the release bundle with the appropriate version number (e.g., `OpenProStreetClient_vX.Y.zip`).

## Git Commit & Push Policy
Whenever you make code modifications and bump the project version, you MUST finish your task by committing and pushing your changes:
1. **Stage Changes**: Run `git add .` (or stage the relevant modified files and new artifacts).
2. **Commit with Version Number**: Run `git commit -m "vX.Y: <brief description of changes>"` where `X.Y` is the new bumped version number.
3. **Push to Remote**: Always run `git push` at the very end of your workflow to push the commit to the remote repository.
