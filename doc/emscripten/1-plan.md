Prompt: Plan the implementation of a `nfd_emscripten.cpp` backend for compatibility with emscripten.

I'll look at the existing API and build setup before writing the plan.

Ran terminal command: cd src; wc -l include/nfd.h nfd_win.cpp nfd_cocoa.m CMakeLists.txt ../CMakeLists.txt; cat CMakeLists.txt | head -120; grep -n "NFD_API\|^typedef\|NFD_.*Enum\|NFD_Path\|NFD_FreePath" include/nfd.h | head -80

Ran terminal command: cd src; sed -n 1,189p CMakeLists.txt | grep -n "PLATFORM\|if(\|endif\|install" | head -60; cat ../CMakeLists.txt | grep -n "PLATFORM"; grep -n "^NFD_API\|^typedef\|NFD_.*_t;\|NFD_.*Enum" include/nfd.h | head -70

Ran terminal command: cd src; sed -n 1,30p ../CMakeLists.txt; sed -n 50,56p include/nfd.h; sed -n 108,135p include/nfd.h; sed -n 78,98p CMakeLists.txt

# Plan: `src/nfd_emscripten.cpp` backend

## Constraints from the browser
- **Sandboxed filesystem.** Browsers never expose real paths. A file or directory picked by the user has to be copied into Emscripten's virtual FS (MEMFS) before C code can see it, so the backend returns paths like `/nfd/<n>/<name>`.
- **Async only.** `<input type=file>` and the File System Access API are promise or event based, and `NFD_*Dialog*` is a blocking, synchronous API. Two routes exist:
  - **Asyncify / JSPI** (`-sASYNCIFY` or `-sJSPI`), which lets a synchronous call suspend. This is the primary target and keeps the existing API unchanged.
  - **Without Asyncify**, a blocking call can't work. The backend returns `NFD_ERROR` with a clear message such as "requires -sASYNCIFY or -sJSPI". Optionally, add a callback-based extension later (see Open questions).
- **User activation.** The picker must be opened from a user gesture such as a click handler. Under Asyncify the call is still inside that gesture, which is fine. Document it.
- **Save dialog.** The browser can't "pick a save location" in a portable way. Two options:
  - Use `showSaveFilePicker` where available (Chromium). It returns a file handle, which we back with a MEMFS file and write back on close.
  - Otherwise fall back to a download via a `Blob` and `<a download>`.
  
  Both need a "commit" step after the app writes the file, which the current API doesn't have. Proposed handling: return a MEMFS path (`/nfd/save/<name>`), and add an optional `NFD_Emscripten_CommitSave(path)` helper that triggers the download or writes through the handle. This is the main design decision and I need your input on it (see Open questions).
- **Folder picking.** Use `<input webkitdirectory>` or `showDirectoryPicker`. Mirror the whole tree into MEMFS and return its root path. This can be expensive for large trees, so document the cost.
- **Unsupported args.** Ignore `parentWindow`, `acceptLabel`, `cancelLabel`, `title` and `defaultPath`, apart from using `defaultPath` as the suggested name for save. Define the handling of `parentWindow.type`.

## Files to change
1. **`src/nfd_emscripten.cpp`** (new). It implements the full API in [nfd.h]():
   - `NFD_Init/Quit`, `NFD_GetError/ClearError`, `NFD_FreePathN/U8`.
   - `NFD_OpenDialog{N,U8}[_With_Impl]`, `…Multiple…`, `NFD_SaveDialog…`, `NFD_PickFolder…`, `NFD_PickFolderMultiple…`.
   - The `NFD_PathSet_*` family. The path set is a `std::vector<std::string>` (or a `malloc`'d array), and the enum is an index wrapper stored in `nfdpathsetenum_t::ptr`.
   - `nfdnchar_t` is `char` here, so the N and U8 variants are identical.
   - Follow the conventions of [nfd_win.cpp]() and [nfd_cocoa.m](): the `NFD_*_With_Impl` version checks, the thread-local error string, and `NFD_ERROR` vs `NFD_CANCEL`.
2. **JS glue**, embedded via `EM_ASYNC_JS` (or `EM_JS` plus `emscripten_sleep` polling for plain Asyncify), in the same file. Responsibilities:
   - Build the `accept` string from `nfdu8filteritem_t`. A spec of `"png,jpg"` becomes `.png,.jpg`. The File System Access API uses `types: [{description, accept: {'*/*': ['.png']}}]`.
   - Create a hidden `<input type=file multiple? webkitdirectory?>`, or call `showOpenFilePicker`/`showDirectoryPicker` when present.
   - Handle cancel. The `cancel` event on input, or an `AbortError` from the picker, maps to `NFD_CANCEL`.
   - Write each file into MEMFS under a unique directory using `FS.mkdirTree` and `FS.writeFile`, reading via `file.arrayBuffer()`. Name collisions are avoided by using a per-call counter directory.
   - Marshal the resulting paths back to C with `stringToNewUTF8`, as `malloc`'d memory so that `NFD_FreePath` uses `free`.
3. **`src/CMakeLists.txt`**:
   - Add `if(nfd_PLATFORM STREQUAL PLATFORM_EMSCRIPTEN)` to append `nfd_emscripten.cpp`.
   - Add `target_link_options(... INTERFACE "-sASYNCIFY")`, behind an option such as `NFD_EMSCRIPTEN_ASYNC=ASYNCIFY|JSPI|OFF`, plus `-sFORCE_FILESYSTEM`.
   - Export the needed runtime methods (`FS`, `stringToNewUTF8`).
4. **Root `CMakeLists.txt`**:
   - Detect `EMSCRIPTEN` before `UNIX` (the Emscripten toolchain sets `UNIX=1`), otherwise it is misdetected as Linux and tries to find GTK/portal.
   - Add `elseif(EMSCRIPTEN) set(nfd_PLATFORM PLATFORM_EMSCRIPTEN)`.
   - Skip the SDL/GLFW and Linux-specific bits.
5. **`test/CMakeLists.txt`**:
   - Build a subset of the tests (`test_opendialog`, `test_opendialogmultiple`, `test_savedialog`, `test_pickfolder`) as `.html` or `.js` outputs.
   - The tests are console-style, so add a small shell page with buttons that trigger them from a click.
   - Skip the native-window and GLFW/SDL variants.
6. **`README.md`**:
   - Add an Emscripten section covering requirements (Asyncify or JSPI), the virtual-FS semantics, the user-gesture rule, browser support, and the save-dialog behavior.
7. **`src/include/nfd.h`**:
   - Possibly add the optional `NFD_Emscripten_CommitSave` declaration, guarded by `#ifdef __EMSCRIPTEN__`, and handle the `parentWindow` type. This is only needed if we go with the commit approach.

## Implementation steps
1. Fix the platform detection in CMake and confirm that a stub file compiles under `emcmake cmake`.
2. Implement the core: error handling, init/quit, the path-set type, and free functions.
3. Implement open (single and multiple) through `<input>` with the filters, then add the File System Access API path as an enhancement.
4. Implement pick-folder (single and multiple) by mirroring the tree into MEMFS.
5. Implement save with the chosen strategy.
6. Add the build options and the Asyncify check. Without Asyncify or JSPI, return a descriptive error.
7. Add the tests and the HTML harness, and run them in a headless browser (Playwright or Chrome) or with Node plus a mock `File`.
8. Update the README and CI. Add an optional GitHub Actions job with `emsdk` that builds the library and tests.

## Validation
- `emcmake cmake -B build-em && cmake --build build-em` builds with no warnings.
- Browser smoke tests in Chromium and Firefox (and Safari if available): open (single and multiple), cancel, folder, and save.
- Confirm that no memory is leaked, using `NFD_FreePath` and `NFD_PathSet_Free`, and that the MEMFS entries for abandoned picks are cleaned up (for example, remove the previous pick's directory in `NFD_Quit` or when its path is freed).

## Open questions
1. **Save semantics:** commit-callback helper (`NFD_Emscripten_CommitSave`), or auto-download when the file is closed via a `FS` hook? The first is simpler and more explicit.
2. **Async strategy:** support Asyncify only, or Asyncify plus JSPI, plus a callback-based API for non-Asyncify builds?
3. **Browser baseline:** should File System Access API usage be optional, with `<input>` as the baseline for Firefox and Safari?

I haven't made any changes yet. Let me know your answers to the questions above, or whether to proceed with the defaults: Asyncify and JSPI, the `<input>` baseline with File System Access as an upgrade, and the explicit commit helper for save.

Claude Sonnet 5.5 • 16.7 credits