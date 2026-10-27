# 将NLang嵌入到其他程序中

在应用里运行NLang脚本有两条路线，本页帮你选。**子进程集成**：以子进程驱动`ncc`与`nvm`——你的程序生成脚本、调用工具、读取输出；这条路线的完整细节（需要带哪些文件、退出码与编码约定、无人值守调试）见[集成NLang](../libraries/integrating-nlang.md)。**进程内嵌入**：宿主C++程序链接`nlang_embed`库，见[嵌入NLang](../libraries/embedding-api.md)。

native扩展的宿主契约（`nlang_<命名空间>.dll`与单一导出入口）也在这条集成线上复用，详见[开发第三方库](../libraries/developing-libraries.md)。

集成面很小：宿主不需要链接任何NLang库，带上安装目录里的几个可执行文件与标准库，用管道和退出码打交道即可。
