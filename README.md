# newperf fio ioengine

`newperf` は、NVMe の性能を直接測定するための外部 fio ioengine です。
Linux のブロックレイヤーを迂回し、BAR0、管理キュー、I/O サブミッションキュー、
I/O コンプリーションキュー、DMA メモリを通じて NVMe コントローラーと通信します。

このエンジンは、学習用途および管理された性能実験を目的としています。
汎用のストレージドライバーではありません。

## 警告

このプログラムは NVMe コントローラーを直接制御します。

- テスト用デバイスのみを使用してください。
- 重要なデータが入っているデバイスでは使用しないでください。
- `write` および `trim` ワークロードはデータを破壊する可能性があります。
- 同じコントローラーを別のドライバーが使用中の状態では実行しないでください。
- 複数の fio ファイルはサポートしていません。
- すべてのジョブは同じコントローラー/名前空間を対象にする必要があります。このエンジンは運用ルールとしてそれを前提にしています。

## 実行前の準備

### 対象デバイスの確認

例として、PCI アドレスが `0000:05:00.0` の NVMe コントローラーを使う場合は、
まず対象デバイスが正しいことを確認してください。

```bash
lspci -nn -s 05:00.0
```

### Linux nvme ドライバーからの unbind

対象の NVMe コントローラーが Linux の `nvme` ドライバーに bind されている場合は、
`newperf` を実行する前に、そのコントローラーを unbind してください。

`driver` symlink が `nvme` を指している場合は、OS の NVMe ドライバーに bind されています。

```bash
readlink /sys/bus/pci/devices/0000:05:00.0/driver
```

unbind するには次を実行します。

```bash
echo 0000:05:00.0 | sudo tee /sys/bus/pci/drivers/nvme/unbind
```

この操作を行うと、対象デバイスは通常の NVMe デバイスとして OS から使用できなくなります。
必ずテスト用デバイスで、対象 PCI アドレスが正しいことを確認してから実行してください。

### genpci ドライバーのインストール

`newperf` は BAR0 と DMA メモリを直接扱うため、付属の `genpci` ドライバーを使用します。
`engines/myplugin` ディレクトリから次を実行して、カーネルモジュールをビルドしてロードしてください。

```bash
cd /home/ttt/fio/engines/myplugin/tools/lib/driver
make
sudo insmod driver.ko
```

モジュール名は `driver.ko` ですが、PCI ドライバー名と作成されるデバイス名は `genpci` です。
ロード後に `/dev/genpci0` のようなデバイスが作成されていることを確認してください。

```bash
ls -l /dev/genpci*
readlink /sys/bus/pci/devices/0000:05:00.0/driver
```

`/dev/genpci*` が作成されない場合は、対象デバイスを `genpci` に手動で bind してください。

```bash
echo 0000:05:00.0 | sudo tee /sys/bus/pci/drivers/genpci/bind
```

テストが終わったら、必要に応じてモジュールを unload します。

```bash
sudo rmmod driver
```

## ビルド

`engines/myplugin` ディレクトリからビルドします。

```bash
cd /home/ttt/fio/engines/myplugin
make
```

これにより次のファイルがビルドされます。

```text
newperf.o
tools/nvme_test
tools/test/test1
tools/test/test2
```

`newperf.o` は、次のコマンドにより共有外部 fio ioengine としてビルドされます。

```bash
gcc -Wall -O2 -g -D_GNU_SOURCE -include ../../config-host.h \
  -shared -rdynamic -fPIC \
  -Itools -Itools/lib -Itools/lib/driver/api -Itools/lib/driver \
  -o newperf.o \
  newperf.c \
  tools/nvme.c \
  tools/lib/ctrl_access.c \
  tools/lib/pci_access.c \
  tools/lib/util.c \
  tools/lib/driver/api/dma.c
```

ビルド生成物を削除して再ビルドするには、次を実行します。

```bash
make clean
make
```

## ターゲット形式

fio の `filename` オプションで NVMe ターゲットを指定します。

```text
<bus>:<device>.<function>,<namespace-id>
```

例:

```text
05:00.0,1
```

fio は `:` をファイル名の区切り文字として扱うため、エスケープしてください。

```bash
--filename='05\:00.0,1'
```

## 基本的な Read テスト

`engines/myplugin` ディレクトリから実行します。

```bash
sudo ../../fio \
  --name=newperf-read \
  --ioengine=./newperf.o \
  --filename='05\:00.0,1' \
  --thread=1 \
  --rw=read \
  --bs=4k \
  --iodepth=32 \
  --size=1G \
  --direct=1 \
  --time_based=1 \
  --runtime=10
```

`--thread=1` は必須です。このエンジンはプロセス内で共有コントローラー状態を使用し、fio スレッドごとに 1 つの SQ/CQ ペアを作成します。

## Write テスト

危険: これはデータを上書きします。

```bash
sudo ../../fio \
  --name=newperf-write \
  --ioengine=./newperf.o \
  --filename='05\:00.0,1' \
  --thread=1 \
  --rw=write \
  --bs=4k \
  --iodepth=32 \
  --size=1G \
  --direct=1 \
  --time_based=1 \
  --runtime=10
```

## Trim テスト

危険: これは LBA を解放します。

```bash
sudo ../../fio \
  --name=newperf-trim \
  --ioengine=./newperf.o \
  --filename='05\:00.0,1' \
  --thread=1 \
  --rw=trim \
  --bs=4k \
  --iodepth=32 \
  --size=1G \
  --direct=1 \
  --time_based=1 \
  --runtime=10
```

使用している fio バージョンが `FIO_MULTI_RANGE_TRIM` をサポートしている場合、マルチレンジ trim を使用できます。

```bash
sudo ../../fio \
  --name=newperf-multi-trim \
  --ioengine=./newperf.o \
  --filename='05\:00.0,1' \
  --thread=1 \
  --rw=trim \
  --bs=4k \
  --iodepth=32 \
  --num_range=8 \
  --size=1G \
  --direct=1 \
  --time_based=1 \
  --runtime=10
```

## 複数ジョブ

プロセスではなく fio スレッドを使用してください。

```bash
sudo ../../fio \
  --name=newperf-read \
  --ioengine=./newperf.o \
  --filename='05\:00.0,1' \
  --thread=1 \
  --numjobs=4 \
  --rw=read \
  --bs=4k \
  --iodepth=32 \
  --size=1G \
  --direct=1 \
  --time_based=1 \
  --runtime=10
```

設計は次のとおりです。

```text
すべての fio スレッドで 1 つのコントローラーを共有
fio スレッドごとに 1 つの I/O SQ
fio スレッドごとに 1 つの I/O CQ
```

キュー ID は fio スレッドごとに割り当てられます。

## 現在の制限事項

- `--thread=1` が必須です。
- `nr_files > 1` は拒否されます。
- すべてのジョブが同じターゲットを使用することを前提としています。
- I/O キュー深度は `iodepth + 1` として設定されます。
- I/O オフセットとサイズは、名前空間の LBA サイズにアラインしている必要があります。
- このエンジンは、fio から提供される I/O バッファーがエンジンの DMA アロケーターを通じて割り当てられていることを前提としています。

## コールバックの流れ

このエンジンは、次の fio コールバック順序に従います。

```text
setup
get_file_size
init
queue / getevents / event
cleanup
```

`setup` は共有コントローラーを初期化します。
`get_file_size` は名前空間サイズを読み取り、fio ファイルサイズを既知としてマークします。
`init` はスレッドごとのキュー状態を割り当て、1 つの SQ/CQ ペアを作成します。

## トラブルシューティング

fio が複数ファイルはサポートされていないと表示する場合は、コロンがエスケープされているか確認してください。

```bash
--filename='05\:00.0,1'
```

fio がシンボル検索エラーを報告する場合は、`engines/myplugin` ディレクトリから `make` を実行して `newperf.o` を再ビルドしてください。

DMA 割り当てに失敗する場合は、`iodepth` または fio のバッファーサイズを小さくしてください。

コントローラーが ready 状態にならない場合は、ターゲットの PCI アドレスが正しいこと、および他のドライバーがそのデバイスを制御していないことを確認してください。
