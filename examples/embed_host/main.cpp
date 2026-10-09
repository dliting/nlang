/*---
    embed_host — 最小 C++ 宿主示例（spec §10；用户手册运行参照）。
    用法：embed_host <artifact.ncu|npkg>
---*/
#include "nlang/embed/NLang.h"
#include <cstdio>
#include <string>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: embed_host <program.ncu|.npkg>\n");
        return 2;
    }
    nlang::initialize();
    nlang::Interpreter itp;

    //A host function the script can call: native int now(); declared in
    //a TU named host.n gives the key "host.now" (declaring-stem rule).
    itp.registerHostFunction("host", "now",
        [](const std::vector<nlang::Value>& args) {
            return nlang::Value(int32_t(42));
        });

    //Show output redirection: prefix everything the script prints.
    itp.setOutputHandler(
        [](const char* text) { std::fprintf(stdout, "[nlang] %s", text); },
        [](const char* text) { std::fprintf(stderr, "[nlang-err] %s", text); });

    try {
        itp.load(argv[1]);
        const int code = itp.run();
        std::fprintf(stdout, "\nexit code: %d\n", code);
        return 0;
    } catch (const nlang::Exception& e) {
        std::fprintf(stderr, "script error: %s\n", e.message().c_str());
        std::fprintf(stderr, "backtrace:\n%s", e.backtrace().c_str());
        return 1;
    } catch (const nlang::LoadError& e) {
        std::fprintf(stderr, "load error: %s\n", e.what());
        return 1;
    }
}
