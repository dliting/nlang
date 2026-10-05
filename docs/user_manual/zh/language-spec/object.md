# Object与装箱


`Object`是class层次结构的隐式根。凡未显式继承其他类的class都隐式继承`Object`。Object由编译器合成——没有源码级的`class Object { ... }`声明，用户也不写`class Foo : Object`（该语法被拒绝）。

`Object`是**引用类型**：`Object`类型的值是一个引用，可指向class对象*或*装箱基本类型。赋值与传参拷贝引用。见[类型语义](type-semantics.md)。

### `equals`与`getHashCode`

Object提供两个带默认恒等语义的虚方法：

```nlang
int equals(Object other);    // 恒等：同一堆引用 → 1，否则 0
int getHashCode();           // 恒等：`this` 的堆索引（null 为 0）
```

**`equals`返回int（0/1），不是bool**——它是可由用户类覆写的虚方法协议，不属于严格bool迁移的比较/谓词面。因此`equals`结果进入条件需要显式比较：`if (a.equals(b) != 0)`（直接写`if (a.equals(b))`是编译错误`if condition must be bool, not "Int32"`）。string的内建`equals`同样返回int。

`equals`与`getHashCode`经按方法名分派实现虚方法——子类只需声明同名方法即可覆写（不需要`override`关键字；运行期沿类层次上溯，最先命中最派生的实现）：

```nlang
class Point {
    public int x;
    public int y;
    int getHashCode() {              // 覆写 Object.getHashCode
        return this.x * 31 + this.y;
    }
}
```

**string的值语义**：string虽是基本类型，但`string.getHashCode()`与`string.equals(string)`调用被内建化为*值*语义（哈希用`std::hash`，equals用内容比较）。这使string无需包装类即可用作`Dict`键。见[字符串](string.md)。

**`==`运算符不受`equals`影响**：Object.equals是可选实现（opt-in）的方法。class引用上的`==`运算符直接比较堆索引。`equals`单独存在的原因，是允许用户类以值相等覆写它，而不破坏更大代码库中恒等相等测试。

### 装箱（基本类型 → Object）

标量基本类型的值（整型家族、`float`/`double`、`bool`、`char`）与`string`赋给Object类型的目标时被隐式装箱：

```nlang
Object o = 5;            // int 装箱
Object f = 3.14;         // double 装箱
Object l = 5000000000;   // long 装箱
Object b = true;         // bool 装箱
Object c = 'é';           // char 装箱
Object s = "hi";         // string 装箱

int TakesObject(Object o) { return o.getHashCode(); }
int x = TakesObject(42); // 42 在调用点装箱
```

运行期表示是带标签的装箱槽位（slot[0] = 类型标签，slot[1] = 值位）。装箱槽位不持有引用，垃圾回收（GC，garbage collection）标记阶段显式跳过它们。

**`null`字面量的装箱保持**：字面量`0`（用作`null`）使`OP_Box`短路——不分配堆槽位，值`0`原样留在Object槽位内容里。这让`Object o = null`与`Object o = 0`都成为无操作，而不是把0包进装箱int的堆引用。

### 拆箱与向下转换

用`as`运算符把基本类型从`Object`拆箱出来，或把`Object`向下转换为class类型。完整`as`规则（拆箱、class向下转换、`Object`向上转换特例、哪些转换是编译错误）见[类型强制转换](type-casts.md)「运行期检查的转换（`as`）」。
