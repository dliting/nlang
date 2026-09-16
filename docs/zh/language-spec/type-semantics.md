# 类型语义


### struct：值语义

struct 在整个语言中遵循值语义：

- **赋值**：`s2 = s1` 创建深拷贝。`s2` 是独立实例——修改 `s2` 不影响
  `s1`。
- **参数传递**：struct 实参深拷贝进被调方的局部栈帧。被调方操作的是
  自己那份副本。
- **返回值**：struct 返回值深拷贝到调用方的结果槽位。
- **class 字段**：struct 作为 class 字段时，class 持有一份独立的深
  拷贝。`obj.s = s1` 会把 `s1` 深拷贝进该 class 的字段槽位。
- **数组元素**（Phase 9d-3）：`new Point[n]` 会急切地为每个元素物化
  一个全新、独立的 struct 实例（含嵌套 struct 字段，递归进行）。把
  元素读入 struct 变量（`Point p = arr[i]`）时深拷贝；经下标写入
  （`arr[i].x = v`、`arr[i] = p`）则存入数组自己的元素。零长度
  struct 数组（`new Point[0]`）合法——`.length` 为 0，不物化任何
  元素。

**struct 内 class 引用的浅拷贝**：struct 含 class 类型字段时，struct
拷贝会原样复制该 class 引用（堆索引）。原件与副本指向堆上同一个
class 对象。这与 C# 对引用类型 struct 字段的行为一致。

示例：
```nlang
class Inner { public int x; }
struct Wrapper { public Inner ref; }

int main() {
    Inner obj = new Inner();
    obj.x = 10;
    Wrapper a;
    a.ref = obj;
    Wrapper b = a;       // 浅拷贝：b.ref == a.ref（同一对象）
    b.ref.x = 99;        // 修改共享的 Inner 对象
    return a.ref.x;      // 返回 99，不是 10
}
```

### class：引用语义

class 遵循引用语义：

- **赋值**：`obj2 = obj1` 复制引用（堆索引）。两个变量指向同一对象。
- **参数传递**：class 实参传递引用。被调方可以修改对象字段，调用方
  能看到改动。
- **返回值**：返回引用，不做拷贝。
- **struct 字段**：class 作为 struct 字段时，struct 存的是引用（堆
  索引）。struct 拷贝会浅拷贝该引用。

### 一览表

| 操作          | struct          | class           |
|--------------------|-----------------|-----------------|
| 赋值         | 深拷贝       | 复制引用  |
| 参数传递  | 深拷贝       | 传递引用  |
| 返回值       | 深拷贝       | 返回引用|
| 作为 class 字段     | 深拷贝且自有 | 存引用 |
| 作为 struct 字段    | 深拷贝且自有 | 存引用 |
