# 命名约定


NLang 方法采用 **camelCase**、类型采用 **PascalCase**——Java 风格大小写
与 C# 风格访问器命名的混合体。这一组合让类型与方法在视觉上立即可分
（`MyClass.myMethod()` 一目了然），也覆盖最大的开发者人群
（Java + JS + C++）。

| 类别 | 风格 | 示例 |
|----------|-------|----------|
| 类型（class / struct / enum / interface） | PascalCase | `MyClass`、`List<T>`、`Color` |
| 方法——动作/命令（有副作用或多参数） | camelCase，裸动词 | `add(x)`、`clear()`、`readInt()`、`run()` |
| 方法——纯访问器（无副作用、无参数、有返回值） | camelCase 加 `get`/`set` 前缀 | `getHashCode()`（为将来的 property 特性保留） |
| 方法——谓词（返回 bool） | camelCase，裸单词 | `equals(o)`、`contains(x)` |
| 自由函数 | camelCase | `print(s)`、`assert(c)` |
| 变量/参数/局部变量 | camelCase | `firstName`、`itemCount` |
| 入口函数 `main` | 小写（唯一例外） | `int main()` |
| 枚举值 | PascalCase | `Color.Red`、`Day.Monday` |
| 泛型类型参数 | 单个大写字母 | `T`、`K`、`V` |
| 私有字段 | camelCase，无前缀 | `class Foo { int count; }` |

**依据**：
- PascalCase 类型 + camelCase 方法 → `MyClass.myMethod()` 让类型与方法的
  区分立即可见；`MyClass.MyMethod()` 则有歧义。
- 覆盖 Java + JS + C++ 约定（最大公约数）。
- `main` 例外保留了 C/C++/Java 通用的入口函数约定。
- 访问器保留 `getXxx` / `setXxx` 前缀：为将来的 property 特性保留命名
  空间（`obj.hashCode` 将脱糖为 `getHashCode()` / `setHashCode(v)`）。
  目前只有 `getHashCode` 使用这一形式；其他访问器（`length`、`count`、
  `position`、`keys`）用裸 camelCase，property 落地后可升级为 `getXxx`。
- 谓词不用 `isXxx` / `hasXxx` 前缀——`equals` 与 `contains` 本身已足够
  清晰。
