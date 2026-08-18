/*--- ProjectFile.h - .nproj project file loader for ncc (-p mode) ---*/
#ifndef NLANG_TOOLS_NCC_PROJECT_FILE_H
#define NLANG_TOOLS_NCC_PROJECT_FILE_H

#include <string>
#include <vector>

namespace nlang {

//Phase 10 Step 0: a compiled unit described by a .nproj XML file.
//  <?xml version="1.0" encoding="UTF-8"?>
//  <Project name="Hello" namespace="hello" outputDir="bin" intermediateDir="obj">
//    <Sources>
//      <File path="main.n"/>
//    </Sources>
//  </Project>
//Paths are relative to the .nproj's directory and resolved to absolute on
//load. namespace/intermediateDir are IDE-facing (Step 1+) and ignored by
//ncc, which only consumes name/outputDir/sources.
struct ProjectFile {
    std::string name;        //module/output name; defaults to the file stem
    std::string projectDir;  //absolute directory of the .nproj file
    std::string outputDir;   //relative output dir ("" -> project dir)
    std::vector<std::string> sources;  //absolute .n paths, project-file order

    //Parses and validates. Returns false with a user-facing errorMessage
    //(missing file, XML error, wrong root, empty Sources, missing path attr).
    static bool Load(const std::string& projectPath, ProjectFile& out,
                     std::string& errorMessage);
};

} //namespace nlang

#endif //NLANG_TOOLS_NCC_PROJECT_FILE_H
