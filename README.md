# DSH-Board firmware — CI staging repo

这个仓库只用于在 GitHub Actions 上跑第一次固件编译。

- `firmware/`：从 `DSH-Board/制作/firmware` 复制过来的 ESP-IDF 工程。
- `.github/workflows/firmware.yml`：用 `espressif/esp-idf-ci-action@v1` 在 `v5.5.1` 下执行 `idf.py build`，并上传 bin。

## 本地状态

已在本地 `git init` + commit（无远程）。

## 推到 GitHub

方式 A：安装并登录 `gh` 后一条命令：

```powershell
gh auth login
gh repo create dsh-board-ci --private --source=. --remote=origin --push
```

方式 B：手动建一个空仓库，然后：

```powershell
git remote add origin https://github.com/<you>/dsh-board-ci.git
git push -u origin main
```

推送后 Actions 会自动跑 `firmware`；也可以手动：

```powershell
gh workflow run firmware.yml
gh run watch
```

## 产物

构建成功后，Actions 的 `dsh-board-firmware` artifact 里包含：

- `firmware/build/dsh_board.bin`
- `firmware/build/bootloader/bootloader.bin`
- `firmware/build/partition_table/partition-table.bin`

## 说明

- 当前环境没有 GitHub 登录（无 `gh`、无 token、无 remote），所以只完成了本地仓库与工作流准备。
- 工作流使用 `espressif/esp-idf-ci-action@v1`；如果该 action 版本不兼容 5.5.1，可改成 `container: espressif/idf:v5.5.1` 直接跑 `idf.py`。
