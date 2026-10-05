# If


```nlang
if (cond) { body }
if (cond) { body } else { body }
```

条件为`true`时执行第一个语句体；否则执行`else`语句体（若存在）。条件必须是
`bool`（见[语句](statements.md)「条件类型」）。`else`之后可跟随另一个`if`
以形成链式判断——`else`语句体就是一个`if`语句；没有独立的`else if`关键
字（单词`elseif`形式是语法错误）。

语句体是复合语句：在其中声明的变量只在该语句体内可见（块作用域）。

```nlang
int n = 7;
if (n > 10) {
    n = 1;
} else if (n > 5) {
    n = 2;
} else {
    n = 3;
}
// n == 2
```
