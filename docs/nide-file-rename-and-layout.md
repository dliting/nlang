# nide: File Rename Pipeline & Window Layout

How a file rename travels through every layer, and how the main window
layout persists across sessions.

## Rename Entry Points

All three entries converge on `MainWindow::renameFileEverywhere(oldPath,
newFileName, trackedFile)`:

- **Tree, in-place edit** — F2 or the tree context menu's 重命名 (F2).
  `SolutionTreeModel::setData` intercepts the EditRole on file rows and
  emits `fileRenameRequested(FileNode*, newName)` — the mirror text is
  never written directly, so a rejected rename leaves no half-moved
  state to roll back.
- **Tree context menu** — 打开 / 重命名 (F2) / 从项目中移除 (Del).
- **Tab context menu** — 保存 / 另存为… / 重命名… / 关闭 / 关闭其他.
  重命名… asks for the new name via `QInputDialog` and resolves the
  tree node with `findFileNodeByPath` (null for standalone files).

## Pipeline Order (and why)

`renameFileEverywhere` runs the steps in a fixed order; each failure
point leaves nothing moved ("zero movement"):

1. **Validate the name** (`fileNameValidationError`: empty, `.`/`..`,
   `<>:"/\|?*`, trailing dot or space). Reject → warning box, nothing
   touched.
2. **Byte-identical name** → no-op success. A case-only variant
   (`main.n` → `Main.n`) is NOT a no-op: it renames for real (the
   domain dedup excludes self and the editor rekey folds case, both
   built and tested for exactly this).
3. **Target exists on disk** → reject — unless the target IS the file
   itself (the case-only variant; `exists()` folds case on Windows).
   The check runs BEFORE the dirty save so a failed rename cannot
   write content anywhere new.
4. **Dirty editor** → save to the OLD path first (decision: save-then-
   rename). Save failure aborts — still zero movement.
5. **`QFile::rename`** on disk. Failure (e.g. a held Windows file
   handle — a read handle blocks renames too) → warning, old path
   intact, editor still keyed to it.
6. **Domain + mirror**: `SolutionTreeModel::renameFile` updates the
   domain node (`ProjectNode::renameFile`, pointer identity preserved,
   dedupKey collision rejects) and the tree item incrementally
   (`setText` — no full refresh, indexes stay valid). Domain rejection
   → the disk rename is ROLLED BACK (`QFile::rename` back).
7. **Editor follows**: `FileEditor::onExternalRename` re-points the
   path, rekeys the manager (lookup under the OLD key, same ordering
   discipline as saveAs), and refreshes the tab title.
8. **Selection** follows the node (`selectFile`).

## Layout

Two central-widget splitters control the window:

| objectName | orientation | default share |
|---|---|---|
| `splitter` | solution tree \| editor side | 20 \| 80 |
| `splitter_2` | code tabs \| output pane | 75 \| 25 |

`QMainWindow::saveState` does NOT cover central-widget splitters, so
each splitter's `saveState()` blob is stored separately in QSettings
(keys `layout/solutionSplitter` and `layout/editorSplitter`; the app
keys under organization "NLang", application "nide").

- **Save**: on an accepted `closeEvent` only — a vetoed close keeps
  the previous state.
- **Restore**: at construction. Garbage bytes, a missing key, or a
  state with any collapsed pane falls back to the default proportions
  and `restoreLayout` reports false.

## 最近打开（File > Recent）

`文件 → 最近打开` 跨会话记忆最近打开的 解决方案 / 项目 / 文件（至多 10 条），
按最近使用排序；条目随打开、新建、另存为入账，重命名在原位更新。
清单持久化在 QSettings（组织 NLang / 应用 nide）；已不存在的文件在菜单中
隐藏但保留在记录里，直到被新条目挤出。「清空最近列表」一键清空。
