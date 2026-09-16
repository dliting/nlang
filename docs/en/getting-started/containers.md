# Crash Course: Containers
### Collections

```nlang
import io;

int main() {
    List<int> nums = [10, 20, 30];  // List initializer
    Dict<string, int> pop =
        new Dict<string, int>{"cn": 14, "us": 3};
    nums.add(40);
    pop["jp"] = 1;                  // write by key
    io.print(nums.length() + pop.count());   // 4 + 3
    io.print(nums[0] + pop["cn"]);           // subscript read
    if (pop.containsKey("us"))
        return 17;
    return 1;
}
```

Output `7` and `24`; exit code 17. `List<T>` grows, `Dict<K,V>` stores
and retrieves by key; both support subscript sugar and `foreach`
iteration.

See also: [Language Specification / Built-in Generic Classes](../language-spec/builtin-generic-classes.md).
