# Crash Course: Containers

This page tours containers through runnable snippets; each section's prose gives the actual output and exit code, ending with a link into the language specification.

### Collections

```nlang
import io;

int main() {
    List<int> nums = [10, 20, 30];  // List initializer
    Dict<string, int> pop =
        {"cn": 14, "us": 3};
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

### Arrays

```nlang
import io;

int main() {
    int[] sizes = [10, 20, 30];  // fixed-length array, list-initialized
    sizes[1] = 25;
    io.print(sizes.length());
    return sizes[2];
}
```

Output `3`; exit code 30. An array's length is fixed at creation; elements
are read and written by subscript. `int[3]` is the other fixed-length
creation form (create first, fill after). In one sentence: fixed
size with value elements — use an array; dynamic growth — use `List<T>`.

See also: [Language Specification / Array](../language-spec/array.md).
