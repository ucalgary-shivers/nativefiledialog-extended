/*
  Native File Dialog Extended
  Repository: https://github.com/btzy/nativefiledialog-extended
  License: Zlib
*/

#include "nfd.h"

#if defined(NFD_EMSCRIPTEN_ASYNCIFY) || defined(NFD_EMSCRIPTEN_JSPI)
#include <emscripten.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

struct JSResult {
    char** paths;
    int count;
};

#if defined(NFD_EMSCRIPTEN_ASYNCIFY) || defined(NFD_EMSCRIPTEN_JSPI)
extern "C" int NFDi_PickJS(int, int, const char*, const char*, JSResult*);
extern "C" char* NFDi_GetLastError(void);
extern "C" int NFDi_CommitSaveJS(const char*);
#endif

namespace {

thread_local char lastError[512]{};

struct PathSet {
    std::vector<std::string> paths;
};

struct Enumerator {
    const PathSet* paths;
    size_t index;
};

void SetError(const char* message) {
    std::snprintf(lastError, sizeof(lastError), "%s", message ? message : "Unknown error");
}

char* CopyPath(const char* path) {
    const size_t length = std::strlen(path) + 1;
    char* copy = static_cast<char*>(std::malloc(length));
    if (copy) {
        std::memcpy(copy, path, length);
    }
    return copy;
}

#if defined(NFD_EMSCRIPTEN_ASYNCIFY) || defined(NFD_EMSCRIPTEN_JSPI)
std::string BuildAccept(const nfdu8filteritem_t* filters, nfdfiltersize_t filterCount) {
    std::string accept;
    for (nfdfiltersize_t i = 0; i < filterCount; ++i) {
        if (!filters[i].spec) {
            continue;
        }
        const char* spec = filters[i].spec;
        while (*spec) {
            while (*spec == ',' || *spec == ';' || *spec == ' ' || *spec == '\t') {
                ++spec;
            }
            const char* end = spec;
            while (*end && *end != ',' && *end != ';' && *end != ' ' && *end != '\t') {
                ++end;
            }
            if (end != spec) {
                std::string extension(spec, end);
                if (extension.compare(0, 2, "*.") == 0) {
                    extension.erase(0, 1);
                } else if (extension[0] != '.') {
                    extension.insert(extension.begin(), '.');
                }
                if (!accept.empty()) {
                    accept.push_back(',');
                }
                accept += extension;
            }
            spec = end;
        }
    }
    return accept;
}
#endif

nfdresult_t Pick(int mode,
                 bool multiple,
                 const nfdu8filteritem_t* filters,
                 nfdfiltersize_t filterCount,
                 const nfdu8char_t* suggestedName,
                 std::vector<std::string>& paths) {
    if (filterCount && !filters) {
        SetError("A nonzero filter count requires a filter list.");
        return NFD_ERROR;
    }

#if defined(NFD_EMSCRIPTEN_ASYNCIFY) || defined(NFD_EMSCRIPTEN_JSPI)
    const std::string accept = BuildAccept(filters, filterCount);
    JSResult result{};
    const int status = NFDi_PickJS(mode,
                                   multiple ? 1 : 0,
                                   accept.c_str(),
                                   suggestedName ? suggestedName : "",
                                   &result);
    if (status < 0) {
        char* jsError = NFDi_GetLastError();
        SetError(jsError ? jsError : "The browser file dialog failed.");
        std::free(jsError);
        for (int i = 0; i < result.count; ++i) {
            std::free(result.paths[i]);
        }
        std::free(result.paths);
        return NFD_ERROR;
    }
    if (status == 0) {
        return NFD_CANCEL;
    }
    paths.reserve(static_cast<size_t>(result.count));
    for (int i = 0; i < result.count; ++i) {
        paths.emplace_back(result.paths[i]);
        std::free(result.paths[i]);
    }
    std::free(result.paths);
    return NFD_OKAY;
#else
    (void)mode;
    (void)multiple;
    (void)filters;
    (void)suggestedName;
    (void)paths;
    SetError("Emscripten dialogs require NFD_EMSCRIPTEN_ASYNC=ASYNCIFY or JSPI.");
    return NFD_ERROR;
#endif
}

nfdresult_t SinglePath(int mode,
                       nfdu8char_t** outPath,
                       const nfdu8filteritem_t* filters,
                       nfdfiltersize_t filterCount,
                       const nfdu8char_t* suggestedName) {
    if (!outPath) {
        SetError("The output path pointer must not be null.");
        return NFD_ERROR;
    }
    *outPath = nullptr;
    std::vector<std::string> paths;
    const nfdresult_t result = Pick(mode, false, filters, filterCount, suggestedName, paths);
    if (result != NFD_OKAY) {
        return result;
    }
    if (paths.empty()) {
        SetError("The browser dialog returned no path.");
        return NFD_ERROR;
    }
    *outPath = CopyPath(paths.front().c_str());
    if (!*outPath) {
        SetError("Could not allocate the output path.");
        return NFD_ERROR;
    }
    return NFD_OKAY;
}

nfdresult_t MultiplePaths(int mode,
                          const nfdpathset_t** outPaths,
                          const nfdu8filteritem_t* filters,
                          nfdfiltersize_t filterCount) {
    if (!outPaths) {
        SetError("The output path-set pointer must not be null.");
        return NFD_ERROR;
    }
    *outPaths = nullptr;
    std::vector<std::string> paths;
    const nfdresult_t result = Pick(mode, true, filters, filterCount, nullptr, paths);
    if (result != NFD_OKAY) {
        return result;
    }
    PathSet* pathSet = new (std::nothrow) PathSet;
    if (!pathSet) {
        SetError("Could not allocate the path set.");
        return NFD_ERROR;
    }
    pathSet->paths = std::move(paths);
    *outPaths = reinterpret_cast<const nfdpathset_t*>(pathSet);
    return NFD_OKAY;
}

}  // namespace

#if defined(NFD_EMSCRIPTEN_ASYNCIFY) || defined(NFD_EMSCRIPTEN_JSPI)
EM_ASYNC_JS(int, NFDi_PickJS,
            (int mode, int multiple, const char* acceptPtr, const char* suggestedNamePtr, JSResult* out),
            {
                const accept = UTF8ToString(acceptPtr);
                const suggestedName = UTF8ToString(suggestedNamePtr);
                Module.nfdLastError = "";
                const writeResult = (paths) => {
                    let arrayPtr = 0;
                    if (paths.length) {
                        arrayPtr = _malloc(paths.length * 4);
                        if (!arrayPtr) throw new Error("Could not allocate browser dialog results.");
                        HEAPU32[out >> 2] = arrayPtr;
                        HEAP32[(out + 4) >> 2] = 0;
                        for (let i = 0; i < paths.length; ++i) {
                            const pathPtr = stringToNewUTF8(paths[i]);
                            if (!pathPtr) throw new Error("Could not allocate a selected path.");
                            HEAPU32[(arrayPtr >> 2) + i] = pathPtr;
                            HEAP32[(out + 4) >> 2] = i + 1;
                        }
                    }
                    HEAPU32[out >> 2] = arrayPtr;
                    HEAP32[(out + 4) >> 2] = paths.length;
                };
                const nextId = () => {
                    Module.nfdSelectionId = (Module.nfdSelectionId || 0) + 1;
                    return Module.nfdSelectionId;
                };
                const ensureDirectory = (path) => {
                    if (!FS.analyzePath(path).exists) FS.mkdirTree(path);
                };
                const safeParts = (name) => name.replace(/\\\\\\\\/g, "/").split("/")
                    .filter((part) => part && part !== "." && part !== "..");
                const writeBrowserFile = async (file, path) => {
                    const slash = path.lastIndexOf("/");
                    ensureDirectory(path.slice(0, slash));
                    FS.writeFile(path, new Uint8Array(await file.arrayBuffer()));
                };
                const inputFiles = () => new Promise((resolve) => {
                    const input = document.createElement("input");
                    input.type = "file";
                    input.multiple = mode === 0 && multiple !== 0;
                    if (mode === 1) input.setAttribute("webkitdirectory", "");
                    if (accept) input.accept = accept;
                    input.style.cssText = "position:fixed;left:-10000px;top:0";
                    document.body.appendChild(input);
                    let done = false;
                    const finish = (files) => {
                        if (done) return;
                        done = true;
                        input.remove();
                        resolve(files);
                    };
                    input.addEventListener("change", () => finish(Array.from(input.files || [])), { once: true });
                    input.addEventListener("cancel", () => finish(null), { once: true });
                    input.click();
                });
                try {
                    if (mode === 2) {
                        let handle = null;
                        const name = safeParts(suggestedName).pop() || "untitled";
                        if (typeof window.showSaveFilePicker === "function") {
                            const extensions = accept.split(",").filter(Boolean);
                            const options = { suggestedName: name };
                            if (extensions.length) {
                                options.types = [{ description: "Allowed files",
                                    accept: { "application/octet-stream": extensions } }];
                            }
                            handle = await window.showSaveFilePicker(options);
                        }
                        const id = nextId();
                        const path = `/nfd/save-${id}/${name}`;
                        ensureDirectory(`/nfd/save-${id}`);
                        FS.writeFile(path, new Uint8Array(0));
                        if (handle) {
                            Module.nfdSaveHandles = Module.nfdSaveHandles || {};
                            Module.nfdSaveHandles[path] = handle;
                        }
                        writeResult([path]);
                        return 1;
                    }

                    if (mode === 1 && typeof window.showDirectoryPicker === "function") {
                        try {
                            const handle = await window.showDirectoryPicker();
                            const id = nextId();
                            const rootName = safeParts(handle.name)[0] || `folder-${id}`;
                            const root = `/nfd/folder-${id}/${rootName}`;
                            const copyDirectory = async (directory, target) => {
                                ensureDirectory(target);
                                for await (const [name, entry] of directory.entries()) {
                                    const parts = safeParts(name);
                                    if (!parts.length) continue;
                                    const child = `${target}/${parts.join("/")}`;
                                    if (entry.kind === "directory") {
                                        await copyDirectory(entry, child);
                                    } else {
                                        await writeBrowserFile(await entry.getFile(), child);
                                    }
                                }
                            };
                            await copyDirectory(handle, root);
                            writeResult([root]);
                            return 1;
                        } catch (error) {
                            if (error && error.name === "AbortError") return 0;
                            throw error;
                        }
                    }

                    const files = await inputFiles();
                    if (!files || !files.length) return 0;
                    const id = nextId();
                    if (mode === 1) {
                        const relative = files[0].webkitRelativePath || files[0].name;
                        const rootName = safeParts(relative)[0] || `folder-${id}`;
                        const root = `/nfd/folder-${id}/${rootName}`;
                        for (const file of files) {
                            const parts = safeParts(file.webkitRelativePath || file.name);
                            if (!parts.length) continue;
                            await writeBrowserFile(file, `/nfd/folder-${id}/${parts.join("/")}`);
                        }
                        writeResult([root]);
                    } else {
                        const root = `/nfd/open-${id}`;
                        const paths = [];
                        for (const file of files) {
                            const name = safeParts(file.name).pop();
                            if (!name) continue;
                            const path = `${root}/${name}`;
                            await writeBrowserFile(file, path);
                            paths.push(path);
                        }
                        writeResult(paths);
                    }
                    return 1;
                } catch (error) {
                    if (error && error.name === "AbortError") return 0;
                    Module.nfdLastError = error && error.message ? error.message : String(error);
                    return -1;
                }
            });

EM_JS(char*, NFDi_GetLastError, (), {
    return stringToNewUTF8(Module.nfdLastError || "The browser file dialog failed.");
});

EM_ASYNC_JS(int, NFDi_CommitSaveJS, (const char* pathPtr), {
    try {
        const path = UTF8ToString(pathPtr);
        const data = FS.readFile(path);
        const handles = Module.nfdSaveHandles || {};
        const handle = handles[path];
        if (handle) {
            const writable = await handle.createWritable();
            await writable.write(data);
            await writable.close();
            delete handles[path];
        } else {
            const filename = path.slice(path.lastIndexOf("/") + 1);
            const url = URL.createObjectURL(new Blob([data]));
            const anchor = document.createElement("a");
            anchor.href = url;
            anchor.download = filename;
            anchor.style.display = "none";
            document.body.appendChild(anchor);
            anchor.click();
            anchor.remove();
            setTimeout(() => URL.revokeObjectURL(url), 1000);
        }
        return 1;
    } catch (error) {
        Module.nfdLastError = error && error.message ? error.message : String(error);
        return 0;
    }
});
#endif

extern "C" {

void NFD_FreePathN(nfdnchar_t* filePath) {
    std::free(filePath);
}

void NFD_FreePathU8(nfdu8char_t* filePath) {
    std::free(filePath);
}

nfdresult_t NFD_Init(void) {
    NFD_ClearError();
    return NFD_OKAY;
}

void NFD_Quit(void) {}

nfdresult_t NFD_OpenDialogN(nfdnchar_t** outPath,
                            const nfdnfilteritem_t* filterList,
                            nfdfiltersize_t filterCount,
                            const nfdnchar_t*) {
    return NFD_OpenDialogU8(outPath, filterList, filterCount, nullptr);
}

nfdresult_t NFD_OpenDialogU8(nfdu8char_t** outPath,
                             const nfdu8filteritem_t* filterList,
                             nfdfiltersize_t filterCount,
                             const nfdu8char_t*) {
    NFD_ClearError();
    return SinglePath(0, outPath, filterList, filterCount, nullptr);
}

nfdresult_t NFD_OpenDialogN_With_Impl(nfdversion_t version,
                                      nfdnchar_t** outPath,
                                      const nfdopendialognargs_t* args) {
    if (version != NFD_INTERFACE_VERSION) {
        SetError("The requested interface version is not supported.");
        return NFD_ERROR;
    }
    if (!args) {
        SetError("The dialog arguments must not be null.");
        return NFD_ERROR;
    }
    return NFD_OpenDialogU8_With_Impl(version, outPath, args);
}

nfdresult_t NFD_OpenDialogU8_With_Impl(nfdversion_t version,
                                       nfdu8char_t** outPath,
                                       const nfdopendialogu8args_t* args) {
    if (version != NFD_INTERFACE_VERSION) {
        SetError("The requested interface version is not supported.");
        return NFD_ERROR;
    }
    if (!args) {
        SetError("The dialog arguments must not be null.");
        return NFD_ERROR;
    }
    NFD_ClearError();
    return SinglePath(0, outPath, args->filterList, args->filterCount, nullptr);
}

nfdresult_t NFD_OpenDialogMultipleN(const nfdpathset_t** outPaths,
                                    const nfdnfilteritem_t* filterList,
                                    nfdfiltersize_t filterCount,
                                    const nfdnchar_t*) {
    return NFD_OpenDialogMultipleU8(outPaths, filterList, filterCount, nullptr);
}

nfdresult_t NFD_OpenDialogMultipleU8(const nfdpathset_t** outPaths,
                                     const nfdu8filteritem_t* filterList,
                                     nfdfiltersize_t filterCount,
                                     const nfdu8char_t*) {
    NFD_ClearError();
    return MultiplePaths(0, outPaths, filterList, filterCount);
}

nfdresult_t NFD_OpenDialogMultipleN_With_Impl(nfdversion_t version,
                                              const nfdpathset_t** outPaths,
                                              const nfdopendialognargs_t* args) {
    if (version != NFD_INTERFACE_VERSION) {
        SetError("The requested interface version is not supported.");
        return NFD_ERROR;
    }
    if (!args) {
        SetError("The dialog arguments must not be null.");
        return NFD_ERROR;
    }
    return NFD_OpenDialogMultipleU8_With_Impl(version, outPaths, args);
}

nfdresult_t NFD_OpenDialogMultipleU8_With_Impl(nfdversion_t version,
                                               const nfdpathset_t** outPaths,
                                               const nfdopendialogu8args_t* args) {
    if (version != NFD_INTERFACE_VERSION) {
        SetError("The requested interface version is not supported.");
        return NFD_ERROR;
    }
    if (!args) {
        SetError("The dialog arguments must not be null.");
        return NFD_ERROR;
    }
    NFD_ClearError();
    return MultiplePaths(0, outPaths, args->filterList, args->filterCount);
}

nfdresult_t NFD_SaveDialogN(nfdnchar_t** outPath,
                            const nfdnfilteritem_t* filterList,
                            nfdfiltersize_t filterCount,
                            const nfdnchar_t*,
                            const nfdnchar_t* defaultName) {
    return NFD_SaveDialogU8(outPath, filterList, filterCount, nullptr, defaultName);
}

nfdresult_t NFD_SaveDialogU8(nfdu8char_t** outPath,
                             const nfdu8filteritem_t* filterList,
                             nfdfiltersize_t filterCount,
                             const nfdu8char_t*,
                             const nfdu8char_t* defaultName) {
    NFD_ClearError();
    return SinglePath(2, outPath, filterList, filterCount, defaultName);
}

nfdresult_t NFD_SaveDialogN_With_Impl(nfdversion_t version,
                                      nfdnchar_t** outPath,
                                      const nfdsavedialognargs_t* args) {
    if (version != NFD_INTERFACE_VERSION) {
        SetError("The requested interface version is not supported.");
        return NFD_ERROR;
    }
    if (!args) {
        SetError("The dialog arguments must not be null.");
        return NFD_ERROR;
    }
    return NFD_SaveDialogU8_With_Impl(version, outPath, args);
}

nfdresult_t NFD_SaveDialogU8_With_Impl(nfdversion_t version,
                                       nfdu8char_t** outPath,
                                       const nfdsavedialogu8args_t* args) {
    if (version != NFD_INTERFACE_VERSION) {
        SetError("The requested interface version is not supported.");
        return NFD_ERROR;
    }
    if (!args) {
        SetError("The dialog arguments must not be null.");
        return NFD_ERROR;
    }
    NFD_ClearError();
    return SinglePath(2, outPath, args->filterList, args->filterCount, args->defaultName);
}

nfdresult_t NFD_PickFolderN(nfdnchar_t** outPath, const nfdnchar_t*) {
    return NFD_PickFolderU8(outPath, nullptr);
}

nfdresult_t NFD_PickFolderU8(nfdu8char_t** outPath, const nfdu8char_t*) {
    NFD_ClearError();
    return SinglePath(1, outPath, nullptr, 0, nullptr);
}

nfdresult_t NFD_PickFolderN_With_Impl(nfdversion_t version,
                                      nfdnchar_t** outPath,
                                      const nfdpickfoldernargs_t* args) {
    if (version != NFD_INTERFACE_VERSION) {
        SetError("The requested interface version is not supported.");
        return NFD_ERROR;
    }
    if (!args) {
        SetError("The dialog arguments must not be null.");
        return NFD_ERROR;
    }
    return NFD_PickFolderU8_With_Impl(version, outPath, args);
}

nfdresult_t NFD_PickFolderU8_With_Impl(nfdversion_t version,
                                       nfdu8char_t** outPath,
                                       const nfdpickfolderu8args_t* args) {
    if (version != NFD_INTERFACE_VERSION) {
        SetError("The requested interface version is not supported.");
        return NFD_ERROR;
    }
    if (!args) {
        SetError("The dialog arguments must not be null.");
        return NFD_ERROR;
    }
    NFD_ClearError();
    return SinglePath(1, outPath, nullptr, 0, nullptr);
}

nfdresult_t NFD_PickFolderMultipleN(const nfdpathset_t** outPaths, const nfdnchar_t*) {
    return NFD_PickFolderMultipleU8(outPaths, nullptr);
}

nfdresult_t NFD_PickFolderMultipleU8(const nfdpathset_t** outPaths, const nfdu8char_t*) {
    NFD_ClearError();
    return MultiplePaths(1, outPaths, nullptr, 0);
}

nfdresult_t NFD_PickFolderMultipleN_With_Impl(nfdversion_t version,
                                              const nfdpathset_t** outPaths,
                                              const nfdpickfoldernargs_t* args) {
    if (version != NFD_INTERFACE_VERSION) {
        SetError("The requested interface version is not supported.");
        return NFD_ERROR;
    }
    if (!args) {
        SetError("The dialog arguments must not be null.");
        return NFD_ERROR;
    }
    return NFD_PickFolderMultipleU8_With_Impl(version, outPaths, args);
}

nfdresult_t NFD_PickFolderMultipleU8_With_Impl(nfdversion_t version,
                                               const nfdpathset_t** outPaths,
                                               const nfdpickfolderu8args_t* args) {
    if (version != NFD_INTERFACE_VERSION) {
        SetError("The requested interface version is not supported.");
        return NFD_ERROR;
    }
    if (!args) {
        SetError("The dialog arguments must not be null.");
        return NFD_ERROR;
    }
    NFD_ClearError();
    return MultiplePaths(1, outPaths, nullptr, 0);
}

const char* NFD_GetError(void) {
    return lastError[0] ? lastError : nullptr;
}

void NFD_ClearError(void) {
    lastError[0] = '\0';
}

nfdresult_t NFD_PathSet_GetCount(const nfdpathset_t* pathSet, nfdpathsetsize_t* count) {
    if (!pathSet || !count) {
        SetError("The path set and count pointer must not be null.");
        return NFD_ERROR;
    }
    *count = static_cast<nfdpathsetsize_t>(static_cast<const PathSet*>(pathSet)->paths.size());
    return NFD_OKAY;
}

nfdresult_t NFD_PathSet_GetPathN(const nfdpathset_t* pathSet,
                                 nfdpathsetsize_t index,
                                 nfdnchar_t** outPath) {
    if (!pathSet || !outPath) {
        SetError("The path set and output path pointer must not be null.");
        return NFD_ERROR;
    }
    const PathSet* paths = static_cast<const PathSet*>(pathSet);
    if (index >= paths->paths.size()) {
        SetError("The path-set index is out of range.");
        return NFD_ERROR;
    }
    *outPath = const_cast<char*>(paths->paths[index].c_str());
    return NFD_OKAY;
}

nfdresult_t NFD_PathSet_GetPathU8(const nfdpathset_t* pathSet,
                                  nfdpathsetsize_t index,
                                  nfdu8char_t** outPath) {
    return NFD_PathSet_GetPathN(pathSet, index, outPath);
}

void NFD_PathSet_FreePathN(const nfdnchar_t*) {}

void NFD_PathSet_FreePathU8(const nfdu8char_t*) {}

nfdresult_t NFD_PathSet_GetEnum(const nfdpathset_t* pathSet, nfdpathsetenum_t* outEnumerator) {
    if (!pathSet || !outEnumerator) {
        SetError("The path set and enumerator pointer must not be null.");
        return NFD_ERROR;
    }
    Enumerator* enumerator = new (std::nothrow) Enumerator{static_cast<const PathSet*>(pathSet), 0};
    if (!enumerator) {
        SetError("Could not allocate the path-set enumerator.");
        return NFD_ERROR;
    }
    outEnumerator->ptr = enumerator;
    return NFD_OKAY;
}

void NFD_PathSet_FreeEnum(nfdpathsetenum_t* enumerator) {
    if (enumerator) {
        delete static_cast<Enumerator*>(enumerator->ptr);
        enumerator->ptr = nullptr;
    }
}

nfdresult_t NFD_PathSet_EnumNextN(nfdpathsetenum_t* enumerator, nfdnchar_t** outPath) {
    if (!enumerator || !enumerator->ptr || !outPath) {
        SetError("The enumerator and output path pointer must not be null.");
        return NFD_ERROR;
    }
    Enumerator* state = static_cast<Enumerator*>(enumerator->ptr);
    if (state->index == state->paths->paths.size()) {
        *outPath = nullptr;
    } else {
        *outPath = const_cast<char*>(state->paths->paths[state->index++].c_str());
    }
    return NFD_OKAY;
}

nfdresult_t NFD_PathSet_EnumNextU8(nfdpathsetenum_t* enumerator, nfdu8char_t** outPath) {
    return NFD_PathSet_EnumNextN(enumerator, outPath);
}

void NFD_PathSet_Free(const nfdpathset_t* pathSet) {
    delete static_cast<const PathSet*>(pathSet);
}

nfdresult_t NFD_Emscripten_CommitSave(const nfdu8char_t* path) {
    if (!path) {
        SetError("The save path must not be null.");
        return NFD_ERROR;
    }
#if defined(NFD_EMSCRIPTEN_ASYNCIFY) || defined(NFD_EMSCRIPTEN_JSPI)
    NFD_ClearError();
    if (NFDi_CommitSaveJS(path)) {
        return NFD_OKAY;
    }
    char* jsError = NFDi_GetLastError();
    SetError(jsError ? jsError : "Could not commit the save file.");
    std::free(jsError);
    return NFD_ERROR;
#else
    SetError("Committing a save requires NFD_EMSCRIPTEN_ASYNC=ASYNCIFY or JSPI.");
    return NFD_ERROR;
#endif
}

}  // extern "C"
