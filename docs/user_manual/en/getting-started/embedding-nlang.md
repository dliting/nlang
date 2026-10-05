# Embedding NLang in Other Programs

To run NLang scripts inside an application today, drive `ncc` and `nvm` as subprocesses: your program generates the script, invokes the tools and consumes their output. Which files to carry, the exit-code and encoding conventions, and unattended debugging are covered in [Integrating NLang](../libraries/integrating-nlang.md).

The host contract for native extensions (`nlang_<namespace>.dll` and its single exported entry) is reused on the same integration line, see [Developing Libraries](../libraries/developing-libraries.md).

The integration surface is small: the host links no NLang library — carry a few executables and the standard library from the install directory, and talk in pipes and exit codes; the minimal file list is in [Integrating NLang](../libraries/integrating-nlang.md).
