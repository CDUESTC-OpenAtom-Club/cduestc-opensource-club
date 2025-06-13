# cduestc-opensource-club
开源社团的相关资料存储，主仓库为https://atomgit.com/cduestc-opensource-club

## 分支说明
- "main"：主分支，存放官方内容，
- "atomgit_source"：同步AtomGit仓库的分支，仅用于代码同步（除README外全量同步）
后续维护（每次同步约5-10分钟）

## 后续维护
### 操作：
 
- 当 AtomGit 仓库更新时，切换到  atomgit_source  分支：
  
```bash
# 拉取 AtomGit 最新代码
git checkout atomgit_source
git pull atomgit main

# 合并代码（强制使用当前分支的非 README 内容）
git merge main -X theirs
# 若有其他文件冲突，手动解决后提交
git push origin atomgit_source

# 可选：将同步内容合并到 main
git checkout main
git merge atomgit_source
```
## 注意事项

1. 确保 AtomGit 和 GitHub 仓库的文件结构一致（除 `README.md` 外）
2. 冲突解决时，严格保留 GitHub `main` 分支的 `README.md` 内容
3. 可通过 GitHub Actions 自动化同步流程（进阶操作，需额外配置）
