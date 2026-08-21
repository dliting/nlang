
CI要考虑的可移植性以及迁移到内网git server平台的问题。

# 结论先说
**GitHub Actions 语法，直接原样搬到内部自建 Git 服务（GitLab CI / Gitea Actions / Jenkins / 自研CI）不能直接运行，通用性很差，但不是“无法迁移”，只是需要改造。**
我们分层拆解：

## 1、根本原因：语法绑定 GitHub Actions 运行时
你这份代码本质是 **GitHub Actions YAML**
```yaml
uses: actions/checkout@v4
runs-on: ubuntu-latest
actions/checkout@v4
```
这些都是**GitHub Actions 专属原语**：
- `uses:` 复用 GitHub Marketplace 的 Action 插件；
- `actions/checkout`：GitHub官方拉代码Action；
- `runs-on: ubuntu-latest`：依赖GitHub托管虚拟机；

内部常见自建Git服务对应的CI体系：
| 内部Git服务 | CI系统 | 是否兼容GitHub Actions yaml |
| ---- | ---- | ---- |
| GitLab CE/EE | GitLab CI（.gitlab-ci.yml） | ❌ 语法完全不一样，不能直接复用 |
| Gitea / Forgejo | Gitea Actions | ✅ **高度兼容GitHub Actions语法**，改动最小 |
| 自建Git + Jenkins | Jenkins Pipeline(Jenkinsfile) | ❌ 需要全部重写 |
| 自研Git平台 | 自研CI | ❌ 几乎全部重构 |

> 重点：**Gitea Actions 是唯一可以几乎直接兼容这份yml的内部方案**；如果你们内部是 GitLab CI，就要大改。

## 2、区分：【Job逻辑】和【CI平台语法】
我们把配置拆成两部分：
### ✅ 可以100%通用、不用修改的**业务逻辑（shell脚本）**
也就是所有 `run:` 里面的命令：
1. bash -n 检查sh脚本语法
2. find 扫描脚本
3. 安装bats
4. validate-manifest.sh 清单校验
5. bats 执行测试

**这一段shell代码，在任何Linux CI环境全部通用，不需要改动！**

### ❌ 无法通用、必须重写的「平台调度层」
```yaml
uses: actions/checkout@v4
runs-on: ubuntu-latest
job名称定义方式
step写法、缓存、artifacts、触发器语法
```
这部分跟着CI平台走。

## 3、分场景给出迁移工作量评估
### 场景A：内部是 Gitea + Gitea Actions（推荐，如果可选）
改动最小：
1. 文件仍然 `.github/workflows/ci.yml` 不用改名；
2. `actions/checkout@v4` 可以正常使用；
3. `runs-on` 需要改成你内部注册的runner标签（不能再写ubuntu-latest，要换成内部runner名称）；
4. 其余shell步骤**完全不用改**。
> 适配成本：低。

### 场景B：内部 GitLab（最常见）
必须**重新写一份 .gitlab-ci.yml**
- 没有 `uses:`，拉代码是GitLab内置行为；
- 没有 `runs-on`，使用 `tags:` 指定runner；
- Job、Stage 定义语法不同；
但是！所有 `run:` 里面的bash命令直接复制粘贴就能用。
适配成本：中等，需要重新搭建CI框架，业务测试脚本复用。

### 场景C：内部Git + Jenkins
需要改成 Jenkinsfile（Groovy），流水线框架全部重写，shell命令保留。
适配成本：较高。

## 4、迁移建议（工程实践）
### 方案1：解耦，提升跨CI通用性（最优）
**把所有测试逻辑封装成独立脚本，剥离CI平台配置**
举个例子：
新建 `scripts/ci/shell-lint-and-test.sh`
把下面所有逻辑全部放进这个脚本：
- 安装bats（或者提前在CI宿主机/镜像预装bats）
- find查找sh文件 + bash -n语法检查
- validate-manifest.sh
- bats执行测试

之后：
- GitHub Actions：调用 `run: bash scripts/ci/shell-lint-and-test.sh`
- GitLab CI：调用 `script: bash scripts/ci/shell-lint-and-test.sh`
- Jenkins：同样执行这一行

👉 **好处：CI配置只负责“调用脚本”，测试逻辑只维护一份，不再和GitHub Actions绑定。**

### 方案2：如果你暂时不想封装脚本
迁移时记住不变原则：
> 所有 `run: |` 内的shell代码 = 通用资产
> 其余yml流水线框架代码 = 平台私有资产

## 5、额外一个坑提醒
你现在CI里 `sudo apt install bats`
迁移内部CI容易遇到问题：
1. 内网机器**无法访问ubuntu官方apt源**；
2. 不允许sudo权限；
解决方案：
- 使用内部apt镜像源；
- 或者在CI基础镜像**预先装好bats**，流水线里不再在线安装。

## 简短总结
1. **原始yml配置不能直接迁移到GitLab/Jenkins；Gitea Actions改动很小；**
2. **真正的测试逻辑（shell命令）完全通用，核心资产不受平台影响；**
3. 长期工程化最佳实践：把所有检查、测试逻辑封装成独立shell脚本，CI流水线只做调用，彻底消除CI平台绑定。
