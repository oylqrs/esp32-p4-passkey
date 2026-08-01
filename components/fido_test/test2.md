**一、先确定 Credential 存储的数据结构**

你这里建议独立做：

```
LittleFS
 |
 +-- /fido/
       |
       +-- credential.db
```

一个 Credential：

```
typedef struct {
    uint8_t credential_id[32];
    uint16_t credential_id_len;
    // P-256 private key
    uint8_t private_key[32];
    // RP
    char rp_id[128];
    // User
    uint8_t user_id[64];
    uint16_t user_id_len;
    char user_name[64];
    // signature counter
    uint32_t sign_count;
} fido_credential_t;
```

实际生产建议：
private_key
     |
     AES-256-GCM加密
     |
LittleFS
不要明文保存。
⸻

二、第一步：单 Credential 保存测试

先不接 CTAP。
写一个测试：

```
生成一个假的Credential
        |
        v
storage_save_credential()
        |
        v
storage_load_credential()
        |
        v
比较数据

```
⸻

测试代码

例如：

```
void test_credential_storage()
{
    fido_credential_t cred1;
    memset(&cred1,0,sizeof(cred1));
    // 模拟credential id
    for(int i=0;i<32;i++)
    {
        cred1.credential_id[i]=i;
    }
    cred1.credential_id_len=32;
    // 模拟private key
    for(int i=0;i<32;i++)
    {
        cred1.private_key[i]=0xA0+i;
    }
    strcpy(
       cred1.rp_id,
       "example.com"
    );
    strcpy(
       cred1.user_name,
       "test"
    );
    cred1.sign_count=0;
    storage_save(&cred1);
    fido_credential_t cred2;
    storage_load(
       cred1.credential_id,
       &cred2
    );
    if(memcmp(
        &cred1,
        &cred2,
        sizeof(cred1)
       )==0)
    {
        printf("STORAGE PASS\n");
    }
    else
    {
        printf("STORAGE FAIL\n");
    }
}
```

⸻

串口：

成功：

[FIDO STORAGE]
save credential
id:
000102030405...
load credential
compare:
PASS

⸻

第三步：多个 Credential 测试
FIDO设备必须支持多个网站。

模拟：
Google
GitHub
Microsoft

保存：
storage_add()

三次。

Flash：
credential.db
[
 credential1
 credential2
 credential3
]

然后：

查询：
storage_find_by_rp()

输入：
github.com

返回：
credential2