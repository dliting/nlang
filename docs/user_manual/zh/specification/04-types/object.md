> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 4.11 Object与装箱

## 4.11.1 根类

`Object`是根类。每个不声明父类的类隐式从`Object`派生；把`Object`显式命名为父类是编译期错误。`Object`类型的值是任意类实例——包括`List`或`Dict`实例——的引用，或`null`。`Object`是引用类型：赋值与传参拷贝引用，默认值是`null`（§4.8.1）。

没有其他类型有通往`Object`的路径：数组、结构体、接口与函数值不可赋值给`Object`，也不能用`as`转换到它（§4.4）。标量基本类型、`string`或枚举值只能通过装箱进入`Object`（§4.11.3）。

*资料性：*`Object`是唯一没有父类的类，内建类——`List`、`Dict`、流类，以及`Exception`与它的内建子类——与用户类共用这一个层次。它的声明位于标准库的预置包，每次构建都自动导入。

## 4.11.2 协议方法

`Object`声明协议成员`equals`、`getHashCode`与`toString`。覆写必须完全一致的规范签名如下：

| 方法 | 规范签名 |
|------|---------|
| `equals` | `equals(Object): int` |
| `getHashCode` | `getHashCode(): int` |
| `toString` | `toString(): string` |

协议方法是虚的，遵循§4.8.9的重写规则：子类声明同名同签名的方法即重写，无需关键字。默认实现比较对象恒等：`equals`在接收者与实参指代同一对象时返回`1`，否则返回`0`；`getHashCode`返回对相等对象相等的`int`。`equals`返回`int`而非`bool`，其结果进入条件需要显式比较（`if (a.equals(b) != 0)`）；直接写`if (a.equals(b))`是编译期错误。

```nlang
class Point {
    public int x;
    public int y;
    int getHashCode() {              // 重写Object.getHashCode
        return this.x * 31 + this.y;
    }
}
```

默认的`toString()`输出`类名@堆位置`（如`Point@1a`）。`as string`转换与字符串位置的隐式转换都调用`toString`（§4.4，§4.5）。

名字与签名匹配某个协议成员的方法，即使声明为`private`或默认访问，也可在类外调用：协议派发按名字查找方法，不检查可见性。访问修饰符在协议声明上照常接受但不产生封装效果；普通成员照常受检（§4.8.2）。

`string`以值语义携带该协议：`string.equals(string)`比较内容，`string.getHashCode()`按内容哈希，这使string无需包装类即可用作`Dict`键（§4.3，§12.2）。

class引用上的`==`不受`equals`重写影响：它比较恒等（§4.8.10），值相等需要显式调用`equals`。

*资料性：*把`equals`保持为独立的、需显式参与的方法，让用户类得以定义值相等而不破坏代码库其余部分的恒等相等测试。string的协议调用被内建化为值语义（内容比较、内容哈希）。

## 4.11.3 装箱

标量基本类型、`string`或枚举值进入`Object`位置时被*装箱*：值被包进一个堆对象，`Object`引用此后指代该对象。装箱在进入点——初始化、赋值或实参——隐式发生，且没有具名的包装类型。枚举按其整数值装箱。

```nlang
Object o = 5;            // int装箱
Object f = 3.14;         // double装箱
Object b = true;         // bool装箱
Object s = "hi";         // string装箱

int TakesObject(Object o) { return o.getHashCode(); }
int x = TakesObject(42); // 42在调用点装箱
```

装箱只在值进入`Object`位置的那一刻发生：`int`局部变量与算术运算始终保持裸值。`null`不装箱——`Object o = null`不分配堆对象；null的引用值就是0本身。

*资料性：*运行期表示是带标签的装箱槽位（首格是类型标签，次格是值位）。装箱槽位不持有引用，垃圾回收标记阶段跳过它们。

## 4.11.4 拆箱与向下转换

把装箱的值读回来需要`as`运算符：`as`把基本类型从`Object`拆箱，并把`Object`向下转换为类类型（§4.5）。从`Object`读回的枚举值给出它所装箱的`int`。完整的`as`规则——拆箱、类向下转换，以及哪些转换是编译期错误——见[类型强制转换](casts.md)。
