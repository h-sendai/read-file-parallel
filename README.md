# read-file-parallel

（自分用メモ）benchmarksに移動しました。

複数のプロセスで同時にファイルを読むときの読み出し速度の計測。

## Usage

```
./read-file-parallel [-i] [-s | -r] [-D] [-t] file0 [file1 ...]
-D: ファイルを読み始めるまえにページキャッシュをドロップしない
-i: O_DIRECTでopenする(読み出しデータをキャッシュしない)
-s: posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL): readaheadサイズを通常の2倍にする
-r: posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM): readaheadを無効化する
-t: read()後の時刻を記録して、ファイル読み出し終了後time.<parent_pid>.<proc_num>ファイルに出力する。
```

[posix_fadvise(2) man page](https://man7.org/linux/man-pages/man2/posix_fadvise.2.html)

## 実装

指定されたファイルごとに1プロセスを使ってopen(), read()する。

ページキャッシュのドロップは``drop-page-cache.c``で実装している。
