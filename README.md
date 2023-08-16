* 在阅读`objc 818.2`版本源码时,发现对`AutoreleasePoolPage`进行了优化.
* `AutoreleasePoolPage`数据结构如下,`AutoreleasePoolPage`类创建是并不是按照类本身所需大小分配, 而是按照固定大小分配
* 根据源码可得知当启用`PROTECT_AUTORELEASEPOOL`,分配大小为`PAGE_MAX_SIZE`否则为`PAGE_MIN_SIZE`,根据系统头文件宏定义可得知`PAGE_MAX_SIZE`为16k `PAGE_MIN_SIZE`为4k
```c++
class AutoreleasePoolPage : private AutoreleasePoolPageData {
    friend struct thread_data_t;

  public:
    static size_t const SIZE =
#if PROTECT_AUTORELEASEPOOL
        PAGE_MAX_SIZE;  // must be multiple of vm page size
#else
        PAGE_MIN_SIZE;  // size and alignment, power of 2
#endif

  private:
    static pthread_key_t const key = AUTORELEASE_POOL_KEY;
    static uint8_t const SCRIBBLE = 0xA3;  // 0xA3A3A3A3 after releasing
    static size_t const COUNT = SIZE / sizeof(id);
    static size_t const MAX_FAULTS = 2;

    // EMPTY_POOL_PLACEHOLDER is stored in TLS when exactly one pool is
    // pushed and it has never contained any objects. This saves memory
    // when the top level (i.e. libdispatch) pushes and pops pools but
    // never uses them.
#define EMPTY_POOL_PLACEHOLDER ((id *)1)

#define POOL_BOUNDARY nil

    // SIZE-sizeof(*this) bytes of contents follow

    static void *operator new(size_t size) {
        return malloc_zone_memalign(malloc_default_zone(), SIZE, SIZE);
    }
    static void operator delete(void *p) {
        return free(p);
    }
};
```
* 假设不启用`PROTECT_AUTORELEASEPOOL`,则一个page对象大小占用4k `AutoreleasePoolPage`实际内存中如下图
<img width="704" alt="截屏2023-08-16 14 43 30" src="https://user-images.githubusercontent.com/14822396/260922691-91899b44-198a-4621-8842-f4687853119c.png">

* `0x000000010080b000`为`AutoreleasePoolPage`对象地址
* `0x000000010080b038`到`0x000000010080F096`这段空间用来存储记录`autorelease`对象地址,每一条记录占用8byte
* 在日常使用中都会成对的在开始调用`AutoreleasePoolPage::push` 结束时调用`AutoreleasePoolPage::pop`
* 而每次`AutoreleasePoolPage::push`调用会在当前page中`add`一个`POOL_BOUNDARY` 并返回对应的地址
* `AutoreleasePoolPage::pop`调用时会传入`AutoreleasePoolPage::push`调用返回的值
```c++
#1
auto token = AutoreleasePoolPage::push(); // 0x000000010080b038
#2
auto object = new Object("test"); // 0x0000600000208000
#3
auto object2 = new Object("test2"); // 0x0000600000208020
#4
AutoreleasePoolPage::autorelease((id)object);
#5
AutoreleasePoolPage::autorelease((id)object2);
#6
AutoreleasePoolPage::autorelease((id)object);
#7
AutoreleasePoolPage::autorelease((id)object);
#8
AutoreleasePoolPage::autorelease((id)object2);
#9
AutoreleasePoolPage::pop(token);
#10
delete object;
#11
delete object2;
```
* 在第`#8`代码执行完后, page内容如下(未优化)
<img width="651" alt="截屏2023-08-16 14 59 19" src="https://user-images.githubusercontent.com/14822396/260926536-d1c756e0-4de1-4abb-b343-e255e3621855.png">

#### 优化后
* 由上面可得知page中每一条记录占用8byte, 优化后每一条记录不再是单纯只记录`autorelease`对象地址, 记录内容变为如下所示
```c++
struct AutoreleasePoolEntry {
	uintptr_t ptr : 48;
	uintptr_t count : 16;

	static const uintptr_t maxCount = 65535;  // 2^16 - 1
};
static_assert((AutoreleasePoolEntry){.ptr = MACH_VM_MAX_ADDRESS}.ptr == MACH_VM_MAX_ADDRESS, "MACH_VM_MAX_ADDRESS doesn't fit into AutoreleasePoolEntry::ptr!");
```
* 48位记录`autorelease`对象地址 16位记录 `autorelease`对象添加次数 注意实际次数为`count + 1`
* 如果是同一个`autorelease`对象连续重复添加,则只需要更新计数
* 还是前面的测试代码, 在第`#8`代码执行完后, page内容如下.
<img width="622" alt="截屏2023-08-16 15 16 54" src="https://user-images.githubusercontent.com/14822396/260930185-f9aaf327-5f27-40ab-88a8-7f56e28fc89c.png">

#### 继续优化
* 在前面的优化基础上又增加了`LRU`策略, 即添加时从`next - 1`处往前最多查找4次是否有待添加的`autorelease`对象记录
* 如果有记录则更新计数,同时将其和当前顶部位置进行交换
* 如果没有记录则按照默认方式添加记录
* 还是前面的测试代码, 在第`#8`代码执行完后, page内容如下.
<img width="714" alt="截屏2023-08-16 15 08 23" src="https://user-images.githubusercontent.com/14822396/260928291-30e25162-555f-4e04-a867-86c281c9864a.png">