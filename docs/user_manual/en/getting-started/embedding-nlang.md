# Embedding NLang in Other Programs

Running NLang scripts inside an application takes one of two routes; this page helps you choose. **Subprocess integration**: drive `ncc` and `nvm` as external commands — your program generates the script, invokes the tools and reads their output. The full details of this route (which files to carry, the exit-code and encoding conventions, unattended debugging) are covered in [Integrating NLang](../libraries/integrating-nlang.md). **In-process embedding**: a host C++ program links the `nlang_embed` library; see [Embedding NLang](../libraries/embedding-api.md).

The host contract for native extensions (`nlang_<namespace>.dll` and its single exported entry) is reused on the same integration line; see [Developing Libraries](../libraries/developing-libraries.md).

The integration surface is small: the host links no NLang library — carry a few executables and the standard library from the install directory, and talk in pipes and exit codes.
