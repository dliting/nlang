# Embedding NLang in Other Programs

Running NLang scripts inside an application takes one of two routes. This page covers the subprocess style — driving `ncc` and `nvm` as external commands: your program generates the script, invokes the tools and consumes their output; for in-process embedding (a host C++ program linking the `nlang_embed` library), see [Embedding NLang](../libraries/embedding-api.md). Which files to carry, the exit-code and encoding conventions, and unattended debugging are covered in [Integrating NLang](../libraries/integrating-nlang.md).

The host contract for native extensions (`nlang_<namespace>.dll` and its single exported entry) is reused on the same integration line; see [Developing Libraries](../libraries/developing-libraries.md).

The integration surface is small: the host links no NLang library — carry a few executables and the standard library from the install directory, and talk in pipes and exit codes.
