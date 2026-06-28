# 哈思 (HaasUI) 系统输入法调用文档

## 概述

哈思设备使用 `global` 模块调用系统输入法。输入法采用事件驱动模式，通过 UUID 识别每次输入会话。

## 核心概念

### 1. 模块导入
```typescript
import globalModule from 'global';

// 获取全局模块实例
const gm = new globalModule.Global();
```

### 2. 关键方法

| 方法 | 说明 | 返回值 |
|------|------|--------|
| `gm.startTextEdit(config)` | 启动输入法 | `string` - UUID |
| `gm.textEditFinished.on(handler)` | 注册结果监听器 | `void` |
| `gm.textEditFinished.off(handler)` | 移除结果监听器 | `void` |
| `gm.closeTextEdit(uuid)` | 关闭输入法 | `void` |

## 完整调用流程

### 步骤 1: 在组件 mounted 时设置监听器

```typescript
mounted() {
    this.onEditFinished();
}
```

### 步骤 2: 定义监听器处理函数

```typescript
onEditFinished() {
    const gm = getGlobalModule();
    
    // 定义回调函数
    this.editFinished = (uuid: string, jsonData: string) => {
        // 关键：检查 UUID 是否匹配
        if (uuid === this.inputTaskUuid) {
            try {
                const result = JSON.parse(jsonData);
                // 只处理确认的情况
                if (result.editConfirmed) {
                    this.editFinishedClicked(result.text);
                }
            } catch (e) {
                console.log('parse error:', e);
            }
        }
    };
    
    // 注册监听器
    if (gm.textEditFinished) {
        gm.textEditFinished.on(this.editFinished);
    }
}
```

### 步骤 3: 启动输入法

```typescript
openKeyboard() {
    const gm = getGlobalModule();
    
    if (gm.startTextEdit) {
        this.inputTaskUuid = gm.startTextEdit(JSON.stringify({
            text: this.currentText,           // 初始文本
            placeholder: '请输入内容',        // 提示文本
            placeholderColor: '#878A99',      // 提示文本颜色
            autofocus: true,                  // 自动聚焦
            maxlength: 100,                   // 最大长度 (-1 为不限制)
            showCursor: true,                 // 显示光标
            cursorColor: '#FF683D',           // 光标颜色
            cursorSize: 3,                    // 光标宽度
            confirmButtonDisabledOnTextEmpty: true,  // 空文本时禁用确认按钮
            inputType: 'ZhCNPreferred',       // 输入法类型
            multiLinesEditVisible: true,      // 允许多行
            capsLockSwitchOn: false,          // 大写锁定
            enterButtonText: '确认'           // 确认按钮文本
        }));
    }
}
```

### 步骤 4: 处理输入结果

```typescript
editFinishedClicked(text: string) {
    const gm = getGlobalModule();
    const cleaned = (text || '').replace(/\n/g, '');
    
    // 处理输入结果
    this.inputText = cleaned;
    
    // 关键：处理完结果后才关闭输入法
    if (gm.closeTextEdit && this.inputTaskUuid) {
        gm.closeTextEdit(this.inputTaskUuid);
    }
    this.inputTaskUuid = '';
}
```

### 步骤 5: 组件销毁时清理

```typescript
beforeDestroy() {
    const gm = getGlobalModule();
    if (gm.closeTextEdit && this.inputTaskUuid) {
        gm.closeTextEdit(this.inputTaskUuid);
    }
    this.inputTaskUuid = '';
}
```

## 输入法配置参数

```typescript
interface TextEditConfig {
    text?: string;                              // 初始文本
    placeholder?: string;                       // 提示文本
    placeholderColor?: string;                  // 提示文本颜色
    autofocus?: boolean;                        // 自动聚焦
    maxlength?: number;                         // 最大长度
    showCursor?: boolean;                       // 显示光标
    cursorColor?: string;                       // 光标颜色
    cursorSize?: number;                        // 光标宽度
    confirmButtonDisabledOnTextEmpty?: boolean; // 空文本时禁用确认
    inputType?: string;                         // 输入法类型
    multiLinesEditVisible?: boolean;            // 允许多行
    capsLockSwitchOn?: boolean;                 // 大写锁定
    enterButtonText?: string;                   // 确认按钮文本
}
```

### 输入法类型 (inputType)

| 值 | 说明 |
|----|------|
| `ZhCNPreferred` | 中文优先 |
| `ZhCNOnly` | 仅中文 |
| `EnUSPreferred` | 英文优先 |
| `EnUSOnly` | 仅英文 |
| `SymbolsPreferred` | 符号键盘 |
| `Number` | 数字键盘 |

## 回调返回数据格式

```json
{
    "editConfirmed": true,
    "text": "用户输入的文本",
    "cursorIndex": 5
}
```

## 重要注意事项

### 1. UUID 匹配检查
每次 `startTextEdit` 都会返回新的 UUID，回调中必须检查 UUID 是否匹配：
```typescript
if (uuid === this.inputTaskUuid) {
    // 处理结果
}
```

### 2. 只处理 editConfirmed 为 true 的情况
```typescript
if (result.editConfirmed) {
    this.editFinishedClicked(result.text);
}
```

### 3. 在处理完结果后才关闭输入法
```typescript
// 在 editFinishedClicked 中关闭
if (gm.closeTextEdit && this.inputTaskUuid) {
    gm.closeTextEdit(this.inputTaskUuid);
}
```

### 4. 不要在回调内部关闭输入法
这会导致系统崩溃！

### 5. 组件销毁时必须清理
```typescript
beforeDestroy() {
    if (gm.closeTextEdit && this.inputTaskUuid) {
        gm.closeTextEdit(this.inputTaskUuid);
    }
}
```

## 完整示例

```typescript
import { getGlobalModule } from '../../utils/keyboard';

export default {
    data() {
        return {
            inputTaskUuid: '',
            editFinished: null as ((uuid: string, jsonData: string) => void) | null,
            inputText: ''
        };
    },
    
    mounted() {
        this.onEditFinished();
    },
    
    beforeDestroy() {
        const gm = getGlobalModule();
        if (gm.closeTextEdit && this.inputTaskUuid) {
            gm.closeTextEdit(this.inputTaskUuid);
        }
        this.inputTaskUuid = '';
    },
    
    methods: {
        onEditFinished() {
            const gm = getGlobalModule();
            
            this.editFinished = (uuid: string, jsonData: string) => {
                if (uuid === this.inputTaskUuid) {
                    try {
                        const result = JSON.parse(jsonData);
                        if (result.editConfirmed) {
                            this.editFinishedClicked(result.text);
                        }
                    } catch (e) {
                        console.log('parse error:', e);
                    }
                }
            };
            
            if (gm.textEditFinished) {
                gm.textEditFinished.on(this.editFinished);
            }
        },
        
        editFinishedClicked(text: string) {
            const gm = getGlobalModule();
            const cleaned = (text || '').replace(/\n/g, '');
            
            this.inputText = cleaned;
            
            if (gm.closeTextEdit && this.inputTaskUuid) {
                gm.closeTextEdit(this.inputTaskUuid);
            }
            this.inputTaskUuid = '';
        },
        
        openKeyboard() {
            const gm = getGlobalModule();
            
            if (gm.startTextEdit) {
                this.inputTaskUuid = gm.startTextEdit(JSON.stringify({
                    text: this.inputText,
                    placeholder: '请输入',
                    placeholderColor: '#878A99',
                    autofocus: true,
                    maxlength: 100,
                    showCursor: true,
                    cursorColor: '#FF683D',
                    cursorSize: 3,
                    confirmButtonDisabledOnTextEmpty: true,
                    inputType: 'ZhCNPreferred',
                    multiLinesEditVisible: true,
                    capsLockSwitchOn: false,
                    enterButtonText: '确认'
                }));
            }
        }
    }
};
```

## 错误排查

### 1. 点击完成闪退
- 检查是否在回调内部调用了 `closeTextEdit`
- 确保只在 `editFinishedClicked` 中调用 `closeTextEdit`

### 2. 输入法无法弹出
- 检查 `global` 模块是否正确导入
- 确保调用 `gm.startTextEdit()` 时传入了正确的 JSON 字符串

### 3. 回调不触发
- 确保在 `mounted` 时注册了监听器
- 检查 UUID 是否正确匹配

---

**参考实现**: 喜马拉雅 (Ximalaya) 应用
