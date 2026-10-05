# 扩展NLang

NLang的扩展点主要是库：把可复用的函数与类型放进一个目录，它就成了可被`import`的包；需要操作系统或现成C++代码时，再配上native实现。写法见[开发第三方库](../libraries/developing-libraries.md)，机制全貌见[库机制](../vm-architecture/library-mechanism.md)。

一个小提示：库不必装进系统目录，用`-I`把任意目录临时加进导入搜索路径即可，详见[ncc](../cli-tools/ncc.md)。

两种形态按需选择：纯NLang库只有源码，拿到就能改；混合库附带native实现，适合封装现成C++代码或直接调用操作系统。
