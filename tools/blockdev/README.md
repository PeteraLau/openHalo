# openHalo 本地块设备存储实验

这个存储管理器让一个指定表空间的普通永久表和索引直接读写 Linux 块设备。默认表空间、系统目录和临时表沿用 md。下面的测试会建立一个新的私有数据库实例和 16 MiB 镜像，在退出时停止实例并解绑 loop 设备。

## 编译与验证

以下命令在 Debian 13 x86-64 上验证过。构建使用普通用户，只有创建和解绑 loop 设备需要管理员权限。依赖包括 Git、GCC、Make、Bison、Flex、pkg-config、uuid-dev、libicu-dev、libreadline-dev、zlib1g-dev、Python 3 和 util-linux。

在仓库根目录运行：

```sh
./configure --prefix="$HOME/openhalo-install" --with-uuid=e2fs --with-icu CFLAGS=-O1
make -j1
make install
sudo bash tools/blockdev/test.sh "$HOME/openhalo-install" "$(id -un)" /var/tmp
```

最后一个参数是存放测试镜像的磁盘目录。脚本每次新建独立目录，只监听该目录下的 Unix socket。脚本会输出本次目录和验收结果，镜像及日志可供复查。

## 存储布局

程序从数据目录下的 `openhalo_block.oid` 读取实验表空间的 OID，从 `openhalo.block` 打开对应设备。测试脚本在数据库停止时配置这两个文件，并为这次实验新建一个带格式标记的镜像。每个集群、每个实验表空间单独使用一个设备。

首个 8 KiB 页保存格式标记和 15 个目录项。每项记录表空间、数据库、关系文件号、fork 编号和逻辑页数，对应一个最多容纳 128 页的连续区域。第 i 项第 b 页的字节偏移为 `8192 + (i * 128 + b) * 8192`，i 和 b 均从 0 起算。删除 fork 时释放目录项，跨过已有末尾扩展时先清零中间页。

在 `src/backend/storage/smgr/blockdev.c` 中，bdinit 读取配置，bdopen 和 bdclose 管理关系的设备句柄，bdcreate 分配区域，bdread 和 bdwrite 定位并传输整页。读操作持有设备共享锁，修改持有排他锁。写入使用 O_DSYNC，在扩展时先完成数据写入，再更新可见长度。上层仍通过 smgr.c 分派。容量耗尽时返回错误。

## 验收结果

插入 1,000 行后，修改 id=500 的内容并删除 99 行。CHECKPOINT 后直接读取块设备可以找到更新后的字符串。重启后从 SQL 再读全表，结果如下。

| 检查 | 重启前 | 重启后 |
| --- | --- | --- |
| 行数 | 901 | 901 |
| id 之和 | 450500 | 450500 |
| 按 id 排序后的内容 MD5 | 691ef5179f8e58700dbbedf240381443 | 691ef5179f8e58700dbbedf240381443 |

脚本还验证主键索引查询、两张表的数据隔离、建表事务回滚、TRUNCATE 后再次插入、DROP 后目录项全部回收，以及普通表仍在文件系统中保存。这个实验按 8 KiB 数据页和固定容量设计。验收覆盖正常停止后的重启；整库物理备份、复制、非正常断电恢复和非持久表恢复不在本次验收范围内。
