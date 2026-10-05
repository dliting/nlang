# Assert


```nlang
assert(condition);
```

求值`condition`。若为假，抛出一个可被`try/catch`块捕获的`AssertionException`。若未捕获，以退出码1终止程序。仅单实参形式（不支持自定义失败消息）。条件必须是`bool`（见[语句](statements.md)「条件类型」）。

```nlang
int x = 5;
assert(x > 3);            // 通过 —— 无事发生
try {
    assert(x > 10);        // 抛出 AssertionException
} catch (AssertionException e) {
    // 被捕获
}
```

见[异常](exception.md)了解`AssertionException`类与`try/catch`机制。
