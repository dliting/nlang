# 速览：容器

本页用可运行片段速览容器；每节正文给出实际输出与退出码，末尾链接语言规格对应章节。

### 集合

```nlang
import io;

int main() {
    List<int> nums = [10, 20, 30];  // List 初始化器
    Dict<string, int> pop =
        {"cn": 14, "us": 3};
    nums.add(40);
    pop["jp"] = 1;                  // 键值写入
    io.print(nums.length() + pop.count());   // 4 + 3
    io.print(nums[0] + pop["cn"]);           // 下标读取
    if (pop.containsKey("us"))
        return 17;
    return 1;
}
```

输出`7`、`24`，退出码17。`List<T>`可增长，`Dict<K,V>`按键存取；两者都支持下标语法糖与`foreach`遍历。

详见 → [语言规格/内建泛型类](../language-spec/builtin-generic-classes.md)。

### 数组

```nlang
import io;

int main() {
    int[] sizes = [10, 20, 30];  // 定长数组，列表初始化
    sizes[1] = 25;
    io.print(sizes.length());
    return sizes[2];
}
```

输出`3`，退出码30。数组的长度在创建时固定，元素按下标读写；`int[3]`是定长创建的另一种写法（先建后填）。选型一句话：尺寸固定、元素按值使用，用数组；要动态增删，用`List<T>`。

详见 → [语言规格/数组](../language-spec/array.md)。
