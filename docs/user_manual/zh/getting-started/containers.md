# 速览: 容器
### 集合

```nlang
import io;

int main() {
    List<int> nums = [10, 20, 30];  // List 初始化器
    Dict<string, int> pop =
        new Dict<string, int>{"cn": 14, "us": 3};
    nums.add(40);
    pop["jp"] = 1;                  // 键值写入
    io.print(nums.length() + pop.count());   // 4 + 3
    io.print(nums[0] + pop["cn"]);           // 下标读取
    if (pop.containsKey("us"))
        return 17;
    return 1;
}
```

输出 `7`、`24`，退出码 17。`List<T>` 可增长，`Dict<K,V>` 按键存取；
两者都支持下标语法糖与 `foreach` 遍历。

详见 → [语言规格/内建泛型类](../language-spec/builtin-generic-classes.md)。

