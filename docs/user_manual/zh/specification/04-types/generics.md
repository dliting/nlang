> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 4.12 泛型

## 4.12.1 泛型类与实例化

用*类型参数*声明的类——`class Box<T> { ... }`、`class Pair<K, V> { ... }`——是*泛型类*。使用泛型类时在名字后的尖括号里给出*类型实参*：`Box<int>`、`Pair<string, int>`。标准库的容器就是泛型类：`List<T>`与`Dict<K, V>`是预置名字，无需导入即可见（§4.12.6）；`Channel<T>`（`concurrent`包）与`Future<T>`（`thread`包）的用法相同。

每个实例化都是一个不同的类型（§4.1）：`List<int>`与`List<string>`不同且互不兼容，`Dict<string, int>`与`Dict<string, long>`亦然。调用按替换后的签名检查：`nums`声明为`List<int>`时，`nums.add("wrong")`是编译期错误。

泛型实例就是普通的类实例：引用类型的值，默认值`null`，可赋值给`Object`（§4.8，§4.11）。

*资料性：*泛型类作为一条模板行只编译一次，所有实例化共享它。容器元素统一以引用形式存储，标量基本类型元素在存入时装箱（§4.11.3）；垃圾回收把容器元素作为附加根追踪。

## 4.12.2 声明泛型类

类型参数写在类名后的尖括号里（§2.4），并在一切可出现类型的位处充当普通类型：字段、形参、返回值与局部类型。成员像其他类一样携带体——方法与构造器针对类型参数只编译一次。泛型类的每个方法必须是`native`或带有方法体。成员名不得与类型参数同名。

```nlang
class Pair<K, V> {
    public K key;
    public V value;
    public Pair(K k, V v) {
        this.key = k;
        this.value = v;
    }
    V get() { return this.value; }
}
```

类型是裸类型参数的字段不能携带初始化器：类型参数没有具体种类，不存在初始值；构造器可以为其赋值。具体类型的字段可以像任何字段一样携带初始化器（§4.8.3）。同理，类型是裸类型参数的形参不能携带默认值（§7.2）。从未赋值的类型参数字段读作实例化时所用类型实参的默认值：整型实参读`0`，`string`或类实参读`null`。

类型参数可以在类型位充当数组元素类型——`T[]`是合法的字段类型——但针对类型参数的数组创建被拒绝：元素种类要到实例化时才已知（§4.10.2）。

*资料性：*泛型成员上的默认实参只在常量表达式形态下随实例化携带——字面量、常量之上的运算符树，或对常量、形参、字段、枚举成员的引用。其他形态——调用表达式或转换——不被携带：经实例化的调用省略这类实参时按缺少实参报错。

## 4.12.3 构造

泛型实例经裸模板名的构造器调用创建，实例化取自声明的目标类型：

```nlang
class Box<T> {
    T value;
    public Box(T v) { this.value = v; }
    public T get() { return this.value; }
}

int main() {
    Box<int> b = Box(7);    // 实例化取自声明的类型
    return b.get();
}
```

裸名本身不携带类型实参：实例化从声明的目标——被初始化的声明、赋值目标或形参类型——读出。没有声明目标的裸名构造器调用是编译期错误（`generic class "Box" cannot be constructed by bare name`）。没有声明构造器的泛型类同样接受零参形态：`Cell<char> cc = Cell();`。

容器还经初始化器形态构造：`List<int> nums = {};`与`List<int> seed = List();`等价，方括号字面量`[1, 2, 3]`按字面量装满列表（见[集合初始化器](../05-expressions/initializers.md)）。

## 4.12.4 嵌套与自引用

类型实参本身可以是实例化：`Box<Box<int>>`、`List<List<int>>`、`Dict<string, List<int>>`。类型实参表内的`>>`拆成两个`>`记号；表外`>>`仍是右移运算符（第5章）。

泛型类可以用相同的类型实参引用自身——`class Node<T> { public Node<T> next; }`——两个泛型类也可以同样互相引用。字段类型嵌套了所声明类的一个新实例化时被拒绝：`class A<T> { public A<A<T>> next; }`报实例化深度错误。

*资料性：*编译器至多铸造16层嵌套实例化；超过上限报`The generic instantiation depth exceeds the limit of 16 nested instantiations.`

## 4.12.5 类型实参

类型实参可以是标量基本类型、`string`、枚举、类类型、接口类型、数组类型，或另一个泛型实例化。`void`类型实参是编译期错误（§4.13）。

用户声明的泛型类还拒绝结构体与函数类型实参：结构体是多槽位的值，而泛型字段只存一个值槽；函数值是多槽位记录（目标、接收者、形态）。内建容器接受结构体类型实参（`Dict`中的结构体键按恒等比较，§4.12.6）。

数组类型实参——`List<int[]>`——把数组值作为裸引用存储；基本类型装箱规则不适用于它们。经`get`或下标取出的元素保留其数组类型，因此`foreach (int[] row in grid)`可直接迭代它们（第6章）。`indexOf`与`contains`按句柄恒等比较数组元素。锯齿类型实参（`List<int[][]>`）与其他锯齿声明一样被拒绝（§4.10.1）。

实例化可以在声明位数组形态中充当元素类型——`List<int> arr[2];`——而表达式位的创建形态`List<int>[2]`是语法错误（§4.10.2）。类型别名可以命名一个实例化（§3.4）：`using IL = List<int>;`之后，`IL`既作类型也用作构造目标。

## 4.12.6 内建容器List与Dict

`List<T>`是可增长、有序、可按索引访问的`T`类型元素集合。

| 方法           | 签名                   | 返回  | 说明                                   |
|----------------|------------------------|-------|----------------------------------------|
| `add`          | `void add(T item)`     | —     | 追加到末尾                             |
| `get`          | `T get(int idx)`       | T     | 按索引读取；越界抛错                   |
| `set`          | `void set(int i, T)`   | —     | 覆写元素                               |
| `length`       | `int length()`         | int   | 当前元素数                             |
| `removeAt`     | `void removeAt(int i)` | —     | 擦除；后继元素前移                     |
| `indexOf`      | `int indexOf(T item)`  | int   | `item`的首个索引，无则-1               |
| `contains`     | `bool contains(T item)` | bool | 存在为true，否则false                  |
| `clear`        | `void clear()`         | —     | 移除全部元素                           |

`get`的索引落在当前长度之外时抛出`IndexOutOfBoundsException`（第9章）。

`Dict<K, V>`是把`K`类型的键映射到`V`类型值的关联数组。

| 方法             | 签名                     | 返回  | 说明                                     |
|------------------|--------------------------|-------|------------------------------------------|
| `set`            | `void set(K key, V value)` | —   | 插入或替换（无重复键错误）               |
| `get`            | `V get(K key)`           | V     | 查找；键不存在时抛错                     |
| `containsKey`    | `bool containsKey(K key)` | bool | 存在为true，否则false                    |
| `remove`         | `int remove(K key)`      | int   | 删除为1，键未找到为0                     |
| `clear`          | `void clear()`           | —     | 移除全部条目                             |
| `count`          | `int count()`            | int   | 当前条目数                               |

对映射中不存在的键调用`get`抛出`Exception`（第9章）；`set`插入或替换，从不因重复键失败。

`Dict`的键相等性区分种类：

- 标量基本类型键比较值位，`NaN`不等于自身；
- `string`键比较内容（§4.3）；
- 类、结构体与数组键按恒等比较——用户的`equals`重写不参与比较（§4.11.2）。

`Dict.keys()`返回装好全部键的新`List<K>`，按键的插入序排列；该列表是*副本*，此后对源dict的`set`/`remove`不影响它。`foreach (K k in dict)`经一次`keys()`快照以同样的方式迭代键（第6章）。不提供`values()`；请迭代键并调用`get`取值。

容器方法按通常规则接受具名实参（§7.2）：`l.add(item = 5)`、`d.set(key = "a", value = 1)`。

经由`null`容器引用调用任何方法，抛出可捕获的`NullPointerException`，消息为`NLang VM: null reference error`——与其他类引用一致（第9章，§4.8.10）。

`T`写`Object`时，容器可以混装不同类型的值：基本类型与`string`在`add`/`set`时隐式装箱，类实例直接存引用，而取出的元素静态类型是`Object`——要用`as`拆箱才能当作具体类型使用（§4.11.4）。`contains`与`indexOf`按值比较装箱的基本类型元素。`K`或`V`写`Object`的`Dict`同理。

```nlang
int main() {
    List<Object> bag = {};
    bag.add(42);                     // int装箱存入
    bag.add("hi");                   // string混装
    int back = bag.get(0) as int;    // 42
    if (back == 42 && bag.contains(42)) {
        return 42;
    }
    return 1;
}
```

*资料性：*`Dict`的每次`set`、`get`、`containsKey`与`remove`都对条目做线性扫描——对典型的小脚本可以接受；不提供O(1)哈希表查找。

## 4.12.7 下标语法糖

`List`上的`li[i]`与`Dict`上的`d[k]`是`get`/`set`方法之上的语法糖，下标赋值是`set`之上的语法糖：

```nlang
List<int> li = {};
li.add(2);
li.add(5);
int v = li[1];          // == li.get(1)   -> 5
li[0] = 9;              // == li.set(0, 9)

Dict<string, int> d = {};
d["a"] = 3;             // == d.set("a", 3)
int x = d["a"];         // == d.get("a")  -> 3
```

越界读写与缺失的dict键的抛错行为与方法形式完全一致（§4.12.6）。由于编译器会从容器的类型剥出元素类型，下标能与语言其余部分自由组合：

```nlang
List<List<int>> m = ...;
m[0][1] = 47;           // 链式下标写

List<Point> pts = ...;
pts[0].x = 9;           // 经下标接收者的成员写

List<int> arr[2];       // 实例化的数组（声明形式）
arr[0] = {};
arr[0].add(1);
arr[0][1];              // 穿过数组元素的容器下标
```

复合下标赋值（`li[0] += 1`）与数组同一策略（第5章，§4.10.5）。

## 4.12.8 跨编译单元的泛型

在一个编译单元声明的泛型类，在另一个单元导入后完全可用：`import boxlib;`之后，`Box<int>`指代所导入模板的一个实例化，构造、成员调用与类型检查和同单元内完全一致。

```nlang
import boxlib;
int main() {
    Box<int> bi = Box(3);
    return bi.get();
}
```

```nlang boxlib.n
class Box<T> {
    T value;
    public Box(T v) { this.value = v; }
    public T get() { return this.value; }
}
```

被导入的函数递归携带真实的形参与返回类型——实例化、数组类型与嵌套数组都在内——所以导入方的调用点检查与声明单元的调用一致。

*资料性：*一个模板的所有实例化共享同一条编译产物；每对象的状态存在实例里。对模板成员的绑定方法引用可用：`func<int> f = b.get;`（§4.13）。
