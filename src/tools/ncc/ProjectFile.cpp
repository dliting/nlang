/*--- ProjectFile.cpp - .nproj project file loader for ncc (-p mode) ---*/
#include "ProjectFile.h"

#include "nlang/compiler/Utf8.h"
#include "tinyxml2.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

namespace fs = std::filesystem;

namespace nlang {

//Dedup key for <File> entries: lexically normalized (folding "sub/../"),
//and on Windows case-folded to match NTFS semantics — entries naming the
//same physical file must collide here, not just identical raw strings.
static std::string SourceKey(const fs::path& abs) {
    std::string key = abs.lexically_normal().string();
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
#endif
    return key;
}

//Header half of Load: reset `out` and read the <Project> attributes
//(name defaults to the file stem, outputDir may be empty).
static void ParseProjectHeader(const tinyxml2::XMLElement* root,
    const fs::path& proj, ProjectFile& out) {
    out = ProjectFile();
    out.projectDir = fs::absolute(proj).parent_path().string();
    out.name = root->Attribute("name") ? root->Attribute("name") : "";
    if (out.name.empty()) {
        //Default to the file stem ("hello.nproj" -> "hello").
        out.name = proj.stem().string();
    }
    out.outputDir = root->Attribute("outputDir")
        ? root->Attribute("outputDir") : "";
}

//<Sources> half of Load: every <File path=...> resolved absolute and
//deduplicated (SourceKey). False after filling errorMessage on the
//first bad entry (missing path / duplicate / no files at all).
static bool CollectSourceFiles(const tinyxml2::XMLElement* sources,
    const std::string& projectPath, ProjectFile& out,
    std::string& errorMessage) {
    std::set<std::string> seen;  //duplicate entries are a user mistake
    for (const tinyxml2::XMLElement* file = sources->FirstChildElement("File");
         file; file = file->NextSiblingElement("File")) {
        const char* path = file->Attribute("path");
        if (!path || !*path) {
            errorMessage = "<File> entry without a path attribute in "
                + projectPath;
            return false;
        }
        fs::path abs = fs::absolute(fs::path(out.projectDir) / path)
            .lexically_normal();
        if (!seen.insert(SourceKey(abs)).second) {
            errorMessage = "duplicate source entry: " + abs.string();
            return false;
        }
        out.sources.push_back(abs.string());
    }
    if (out.sources.empty()) {
        errorMessage = "project has no source files: " + projectPath;
        return false;
    }
    return true;
}

//<ImportPaths> half of Load: each <Dir path=...> resolved absolute against
//the project dir. A missing/empty path is an error. The block is optional;
//de-duplication across the full path is left to BuildLibrarySearchPath.
static bool CollectImportDirs(const tinyxml2::XMLElement* importPaths,
    const std::string& projectPath, ProjectFile& out,
    std::string& errorMessage) {
    for (const tinyxml2::XMLElement* dir =
             importPaths->FirstChildElement("Dir");
         dir; dir = dir->NextSiblingElement("Dir")) {
        const char* path = dir->Attribute("path");
        if (!path || !*path) {
            errorMessage = "<Dir> entry without a path attribute in "
                + projectPath;
            return false;
        }
        fs::path abs = fs::absolute(fs::path(out.projectDir) / path)
            .lexically_normal();
        out.importPaths.push_back(abs.string());
    }
    return true;
}

//Validate the single <Sources> block and collect its files.
static bool CollectSourcesBlock(const tinyxml2::XMLElement* root,
                                const std::string& projectPath,
                                ProjectFile& out, std::string& error) {
    const tinyxml2::XMLElement* sources =
        root->FirstChildElement("Sources");
    if (!sources) {
        error = "project file has no <Sources> element: " + projectPath;
        return false;
    }
    //A second block would silently drop files, so reject it.
    if (sources->NextSiblingElement("Sources")) {
        error = "project file has more than one <Sources> element: "
            + projectPath;
        return false;
    }
    return CollectSourceFiles(sources, projectPath, out, error);
}

//Validate the optional single <ImportPaths> block and collect its dirs.
static bool CollectImportPathsBlock(const tinyxml2::XMLElement* root,
                                    const std::string& projectPath,
                                    ProjectFile& out, std::string& error) {
    const tinyxml2::XMLElement* importPaths =
        root->FirstChildElement("ImportPaths");
    if (!importPaths)
        return true;   // optional
    if (importPaths->NextSiblingElement("ImportPaths")) {
        error = "project file has more than one <ImportPaths> element: "
            + projectPath;
        return false;
    }
    return CollectImportDirs(importPaths, projectPath, out, error);
}

//Byte half of Load: read the raw file and enforce the same UTF-8 input
//contract as source files (before XML parsing — a legacy-encoded .nproj
//used to smuggle mojibake names into the build or crash later; a UTF-8
//BOM is accepted and skipped, UTF-16 saves get a dedicated hint).
//False after filling errorMessage.
static bool ReadProjectContent(const std::string& projectPath,
                               std::string* content,
                               std::string& errorMessage) {
    std::ifstream stream(projectPath.c_str(),
                         std::ios::in | std::ios::binary);
    if (!stream) {
        errorMessage = "cannot read project file: " + projectPath;
        return false;
    }
    std::string raw((std::istreambuf_iterator<char>(stream)),
                    std::istreambuf_iterator<char>());
    const char* validated = nullptr;
    size_t validatedLength = 0;
    std::string reason;
    if (!Utf8ContentCheck(raw.data(), raw.size(), &validated,
                          &validatedLength, &reason)) {
        errorMessage = "Project file " + projectPath + " " + reason
            + ". Save the file as UTF-8.";
        return false;
    }
    content->assign(validated, validatedLength);
    return true;
}

bool ProjectFile::Load(const std::string& projectPath, ProjectFile& out,
                       std::string& errorMessage) {
    fs::path proj(projectPath);
    if (!fs::exists(proj)) {
        errorMessage = "project file not found: " + projectPath;
        return false;
    }

    std::string content;
    if (!ReadProjectContent(projectPath, &content, errorMessage))
        return false;

    tinyxml2::XMLDocument doc;
    if (doc.Parse(content.data(), content.size())
        != tinyxml2::XML_SUCCESS) {
        errorMessage = "XML parse error in " + projectPath + ": "
            + doc.ErrorStr();
        return false;
    }

    const tinyxml2::XMLElement* root = doc.RootElement();
    if (!root || std::string(root->Name()) != "Project") {
        errorMessage = "not a project file (root element must be <Project>): "
            + projectPath;
        return false;
    }

    ParseProjectHeader(root, proj, out);
    if (!CollectSourcesBlock(root, projectPath, out, errorMessage))
        return false;
    if (!CollectImportPathsBlock(root, projectPath, out, errorMessage))
        return false;
    return true;
}

} //namespace nlang
