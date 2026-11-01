> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 12. 集合

`List<T>`与`Dict<K, V>`是两个内建容器类：预置包声明的泛型类（§4.12.1、§4.12.6）。本章定义它们的方法面——每个方法的签名、效果与失败行为——元素与键如何匹配，以及以`Object`实例化时容器如何混装不同类型的值。类型参数规则、构造与下标语法糖留在泛型一章（§4.12.2–§4.12.5、§4.12.7）。

经由`null`容器引用调用任何方法，抛出可捕获的`NullPointerException`，消息为`NLang VM: null reference error`——与其他类引用一致（§4.8.10、§8.3）。容器的存入或取出按§9.1的类别移动值。下标形式`li[i]`与`d[k]`是`get`与`set`的语法糖（§5.7、§4.12.7），容器方法按通常规则接受具名实参（§7.2）。

## 12.1 List

`List<T>`（§4.12）是可增长、有序、可按索引访问的`T`类型元素序列。

| 方法 | 签名 | 效果 | 失败行为 |
| --- | --- | --- | --- |
| `add(value)` | `void add(T value)` | 追加`value` | — |
| `get(i)` | `T get(int i)` | 位置`i`处的元素 | 越界抛出`IndexOutOfBoundsException` |
| `set(i, value)` | `void set(int i, T value)` | 替换`i`处的元素 | 越界抛出`IndexOutOfBoundsException` |
| `length()` | `int length()` | 元素数 | — |
| `removeAt(i)` | `void removeAt(int i)` | 移除`i`处的元素，后续元素下移 | 越界抛出`IndexOutOfBoundsException` |
| `indexOf(value)` | `int indexOf(T value)` | 匹配`value`的首个位置，没有则为`-1` | — |
| `contains(value)` | `bool contains(T value)` | 是否有某个位置匹配`value` | — |
| `clear()` | `void clear()` | 移除所有元素 | — |
| `toString()` | `string toString()` | `[e1, e2, ...]`（§11.2） | — |

`indexOf`与`contains`按区分种类的相等性匹配：

- 标量基本类型元素比较值位，`NaN`不等于自身；
- `string`元素比较内容（§4.3）；
- 类、结构体与数组元素按恒等比较——用户的`equals`重写不参与比较（§4.11.2）。

同样的匹配决定`Dict`的键（§12.2）。

`T`写`Object`时，列表可以混装不同类型的值：基本类型与`string`在`add`时隐式装箱，类实例直接存引用，而取出的元素静态类型是`Object`——要用`as`拆箱才能当作具体类型使用（§4.11.4）。`contains`与`indexOf`按值比较装箱的基本类型元素。

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

## 12.2 Dict

`Dict<K, V>`（§4.12）把`K`类型的键映射到`V`类型的值。

| 方法 | 签名 | 效果 | 失败行为 |
| --- | --- | --- | --- |
| `set(key, value)` | `void set(K key, V value)` | 把`value`存到`key`下；重新存一个键会替换值并保持该键的位置 | — |
| `get(key)` | `V get(K key)` | `key`下所存的值 | `key`没有条目抛出`Exception` |
| `containsKey(key)` | `bool containsKey(K key)` | `key`的条目是否存在 | — |
| `remove(key)` | `int remove(K key)` | 移除`key`的条目；移除了产出`1`，键未找到产出`0` | — |
| `clear()` | `void clear()` | 移除所有条目 | — |
| `count()` | `int count()` | 条目数 | — |
| `keys()` | `List<K> keys()` | 一个新的`List<K>`，含按插入序的各键 | — |
| `toString()` | `string toString()` | `{k1: v1, k2: v2, ...}`，按插入序（§11.2） | — |

键匹配就是§12.1的区分种类的相等性。内建容器接受结构体类型实参，因此结构体键按恒等比较（§4.12.5）。

`keys()`是整个dict接口的顺序保证：返回的列表按插入序持有全部键——把一个值存到已经存在的键下不会移动该键（§6.4）——且该列表是副本，此后对源dict的`set`与`remove`不影响它。`foreach (K k in dict)`经一次`keys()`快照以同样的方式迭代键（§6.8），`toString`也按同一顺序渲染条目（§11.2）。不提供`values()`；请迭代键并调用`get`取值。

`K`或`V`写`Object`时，`List`的装箱规则同样适用（§12.1）。

*资料性：*`Dict`的每次`set`、`get`、`containsKey`与`remove`都对条目做线性扫描——对典型的小脚本可以接受；不提供O(1)哈希表查找。
