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

判等运算符对类型参数操作数是合法的：`==`与`!=`作用于`T`与`T`、`T`与`null`（两个方向）时，方法体在运行期读取接收者对象携带的实例化信息，按实例化时的实参种类分派——标量按内联值比较，`string`按内容，结构体按字段值，类与数组按恒等，枚举按内联值。`null`一侧按解析出的种类做引用域判空（标量实参的值永不为`null`）。拼接与打印同样按实参种类转换：`"v=" + item`与`item + "!"`两序都把`T`值经其实际种类的字符串形态（标量渲染、字符串本身、结构体的`<struct>`、类的虚`toString`、枚举的数值、数组的格式遍历）并入结果，`print`直印同理。其余依赖种类的运算符——关系与算术——维持编译期拒绝，报`operator '...' cannot be applied to a type parameter: the operand kind is not known until the instantiation.`。转型能换回具体种类：已知实例化时`item as string == "a"`重新按内容比较。`T`值的存储、传参与返回不依赖种类，保持合法。

上述分派都在实例方法或构造器体内进行——它们读取的实例化信息来自接收者对象（`this`）。`static`成员的帧没有接收者，对`T`的判等、拼接、打印与`as`转型在`static`上下文一律拒绝，报`... requires an instance context.`（§4.12.9）。

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

类型实参可以是标量基本类型、`string`、枚举、类类型、接口类型、结构体类型、数组类型，或另一个泛型实例化。`void`类型实参是编译期错误（§7.4）。

用户声明的泛型类拒绝函数类型实参：函数值是多槽位记录（目标、接收者、形态）。结构体类型实参被接受——每个边界（构造函数形参、局部绑定、字段存储、`foreach`）都经替换类型深拷贝记录，字段的值槽位持有一份私有副本。内建容器同样接受结构体类型实参（`Dict`中的结构体键按字段值比较，§12.2）。

结构体实参有一个角落：在泛型类自身的方法体内，两个`T`类型字段之间的`T`到`T`搬运（`item = other.item;`）不经拷贝地转移记录句柄，两个字段随即共享同一记录——经一方的写入会透传到另一方。只有体内字段搬运会发生别名；每个边界都深拷贝（§4.7.4）。

数组类型实参——`List<int[]>`——把数组值作为裸引用存储；基本类型装箱规则不适用于它们。方括号拼写与泛型拼写是同一类型（§4.10.1），`List<Array<int>>`与`List<int[]>`是一类型两写法。经`get`或下标取出的元素保留其数组类型，因此`foreach (int[] row in grid)`可直接迭代它们（第6章）。`indexOf`与`contains`按句柄恒等比较数组元素。锯齿类型实参（`List<int[][]>`）与其他锯齿声明一样被拒绝（§4.10.1）——泛型服装下的同一锯齿形状同样如此：`Array<Array<int>>`就是`int[][]`，无论写在何处都是编译期错误。无标注的`Array<T>`是普通的类型实参：泛型类内的`List<Array<T>>`声明类型参数的数组列表。

实例化可以在声明位数组形态中充当元素类型——`List<int> arr[2];`——而表达式位的创建形态`List<int>[2]`是语法错误（§4.10.2）。类型别名可以命名一个实例化（§3.4）：`using IL = List<int>;`之后，`IL`既作类型也用作构造目标。

## 4.12.6 内建容器List与Dict

`List<T>`是可增长、有序、可按索引访问的`T`类型元素集合，`Dict<K, V>`是把`K`类型的键映射到`V`类型值的关联数组。两者都是预置名字，无需导入即可见（§3.9.4），也是普通的泛型类：每个实例化都是相异的类型（§4.12.1），构造遵循§4.12.3，类型实参遵循§4.12.5。

方法面——每个方法的签名、效果与失败行为，元素与键的区分种类的相等性匹配，`Dict`键的插入序，以及以`Object`实例化时混装不同类型的值——由[第12章 集合](../library/12-collections.md)定义。

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

越界读写与缺失的dict键的抛错行为与方法形式完全一致（§12）。由于编译器会从容器的类型剥出元素类型，下标能与语言其余部分自由组合：

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

*资料性：*单元同时携带一张模板表——每行是该单元注册的一个实例化（规范拼写与构造配方）。链接时各单元的行合并；跨单元构造的对象照常携带实例化信息（§4.12.9），判等、转型与`typeName`的行为跨单元一致。

*资料性：*一个模板的所有实例化共享同一条编译产物；每对象的状态存在实例里。对模板成员的绑定方法引用可用：`func<int> f = b.get;`（§7.4）。实例化在运行期不是透明的：每个由实例化构造的对象都携带一份*实例化信息*——模板名与逐实参的种类和规范拼写——供判等分派、转型与`typeName`读取（§4.12.2，§4.12.9）。

## 4.12.9 运行时类型信息与类型参数

`typeName`以类型表达式为实参，返回它的规范拼写字符串。具体类型的拼写编译期折叠——`typeName(int)`是`"int"`，`typeName(Box<int>)`是`"Box<int>"`，`typeName(int[,])`是`"int[,]"`（秩进括号），`typeName(List<List<int>>)`是`"List<List<int>>"`。含类型参数的形态在运行期经接收者的实例化信息解析：

```nlang
class Box<T> {
    public T value;
    public Box(T v) { this.value = v; }
}
class Hold<T> {
    public string showArg() { return typeName(T); }        // "int" / "Box<int>"
    public string showInner() { return typeName(Box<T>); } // "Box<int>"
    public string showArray() { return typeName(T[]); }    // "int[]" / "Box<int>[]"
}
```

裸`T`读接收者实参的拼写；`T`嵌在实参内（`Box<T>`、`T[]`）按同样的实参逐层组合，含类型参数的数组把元素拼写接上括号对。与判等同源，`typeName`的运行期形态要求实例上下文，`static`成员中拒绝。`typeName`是保留字（§2.4）。

方法体内以类型参数为实参构造实例化——`List<T> items = {};`——与具体实例化共用一套构造路径：对象携带的实例化信息在构造点按配方组合（`T`取自接收者实参，数组包装元素配方，嵌套照此递归），之后这个对象的判等、转型与`typeName`行为与写出具体类型的实例完全一致。

`as`转型把类型参数当作目标时——`o as T`——按接收者实参的种类分派到既有转型运行时：装箱标量与`string`拆箱，结构体做行检查，类沿超类链并比较实参自身的实例化，数组按元素类型，枚举沿整型值。目标不符时抛可捕获的`InvalidCastException`，消息写明目标拼写；`null`操作数穿透。转型区分实例化：`Box<int>`的实例不能转成`Box<string>`，即使两者共享同一模板。已知实例化的转型（`item as string`）不受此节约束，按§5的正常转型规则。

`catch`的类型位不接受泛型实例化——包括类型参数本身：`catch (Box<int> e)`是编译期错误（`catch on a generic instantiation type ... cannot tell instantiations apart`）。异常分派沿模板行匹配，无法区分同一模板的不同实例化，接受拼写会许诺运行期做不到的区分（§8.3）。

*补偿模式：*需要按类型参数记录不同实例化的场合，用一个非泛型辅助类持有以拼写为键的字典，泛型类在实例方法体内把`typeName(T)`作为键传入：

```nlang
class TypeCounter {
    private static Dict<string, int> counts = {};
    public static void bump(string key) {
        if (counts.containsKey(key)) { counts.set(key, counts.get(key) + 1); }
        else { counts.set(key, 1); }
    }
    public static int count(string key) {
        if (counts.containsKey(key)) { return counts.get(key); }
        return 0;
    }
}
class Holder<T> {
    public void note(T item) {
        TypeCounter.bump(typeName(T));   // 每个实例化一个键
    }
}
```

键是`typeName(T)`的规范拼写，因此`Holder<int>`与`Holder<Box<int>>`各占一档。
