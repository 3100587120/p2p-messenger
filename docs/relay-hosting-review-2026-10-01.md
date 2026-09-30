# 香港/大陆免费中继候选核查（2026-10-01）

## 结论与真实部署状态

腾讯 EdgeOne Makers 是目前核查到的优先候选：官方列有长期免费套餐，Cloud Functions 可配置 `ap-hongkong`（中国香港），大陆区域也可选。**本次未完成新主中继上线，客户端当前仍使用原 Cloudflare 服务。** 未把新地址虚构为可用，也未修改或重置 UID、密码和好友关系。

不能承诺任何供应商永远免费、永不下线。套餐政策不等于本项目已拿到可部署资源、适配并通过验收。

## 为什么不直接替换域名

现有中继依赖一个有持久化存储的单一 Router，同时拥有全部 WebSocket 连接。普通云函数按请求扩缩容，两个客户端不一定在同一进程；仅放一个内存 Map，会再次出现双方在线但互相收不到消息。EdgeOne Cloud Functions 官方单次请求默认 30 秒、最大 120 秒。需要确认 WebSocket 生命周期、跨实例投递和并发账号注册的原子性。

EdgeOne KV 跨节点最终一致，缓存最多 60 秒，不适合直接照搬注册顺序 UID 或即时投递。Blob 支持强一致读，但强一致读本身不等于并发事务或比较交换锁，不能凭此宣称顺序 UID 不会重复。

## 部署与切换门槛

1. 用户登录 EdgeOne 控制台，确认免费权益、香港函数区域和服务条款，不启用付费或自动扣费。
2. 完成独立中继的跨实例连接与账号存储适配；不能简单把 Cloudflare 反代包装成独立香港中继。
3. 备份并原样迁移账号目录（UID 顺序、1–10 预留、密码验证信息、加密身份备份、加密好友备份、修复记录）。禁止为迁移强迫用户重新注册。
4. 用实际部署的香港端点测试 WSS、好友申请、UID 登录、私聊/群聊、语音和文件；保留 Cloudflare 回滚能力。域名接入节点不等于真正处理消息的后台位置。
5. 通过后再内置主端点及备用逻辑；用户自定义中继仍只覆盖本机配置。

本次只做候选核查，不在缺少登录和跨实例验收时启动账号迁移。

## 其他候选

- 腾讯 CloudBase 免费环境需每 6 个月手动续期，免费函数时长和推送连接数受限，并非无需维护的永久常驻主机。
- Oracle 有 Always Free，但官方商业 OCI 区域列表目前未列大陆/香港/澳门/台湾，不能以其他亚洲区域冒充。
- Render 免费区域没有大陆/港澳台，且有休眠、冷启动和临时文件丢失问题，不符合本次主中继条件。

## 官方依据

- [EdgeOne Makers 价格](https://pages.edgeone.ai/pricing)
- [香港与大陆函数区域及限制](https://pages.edgeone.ai/document/cloud-functions)
- [函数配置](https://pages.edgeone.ai/document/edgeone-json)
- [KV 一致性](https://pages.edgeone.ai/document/kv-storage)
- [Blob 一致性](https://edgeone.ai/document/210063146417373184)
- [CloudBase 免费续期和额度](https://cloud.tencent.com/document/product/876/127357)
- [OCI 商业区域](https://www.oracle.com/tw/cloud/public-cloud-regions/)
- [Render 区域](https://render.com/docs/regions)、[免费限制](https://render.com/docs/free)
