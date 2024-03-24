# 无锁查找表的C++简单实现

![image.png](https://s2.loli.net/2024/03/24/4sFEunVXLYKiW7y.png)

## 需求

设计一个性能优化的查找表looktable.
问题：
交易程序下单的时候，交易所会返回int64_t order_id, order_id在一天内是时间递增的， 大部分的order生命周期都是短暂，如果order生命结束，对它就失去兴趣了，可以从looktable删掉。
要求：
   1. 只需要满足一天的需求，order_id范围100万级别。
   2. insert, erase不要频繁分配内存(比如malloc, new)
   3. 至少提供一个模板参数T, T可能保存order的相关信息。