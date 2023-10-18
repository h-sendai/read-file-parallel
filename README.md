# read-file-parallel

複数のプロセスで同時にファイルを読むときの読み出し速度の計測。

## Usage

```
./read-file-parallel [-D] file0 [file1 ...]
-D: ファイルを読み始めるまえにページキャッシュをドロップしない
```

## 実装

指定されたファイルごとに1プロセスを使ってopen(), read()する。

ページキャッシュのドロップは``drop-page-cache.c``で実装している。
