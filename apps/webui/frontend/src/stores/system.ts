import { defineStore } from 'pinia'
import { ref } from 'vue'
import { systemApi } from '@/api/system'

/**
 * 系统信息 store：缓存后端进程名（exe_stem）与工程版本串
 *
 * 用途：
 * - `processName`：前端比对 logger 名，禁用 WebUI 自身日志的实时 tail（避免反馈循环）；
 *   后端 ws_controller 与 log_controller 也通过 exe_stem() 动态判断，前后端双重 guard。
 *   支持重命名 dzweb.exe 部署。策略进程 / 监控进程的类型推导也依赖它（stores/logs.ts）。
 * - `version`：后端自述的工程版本（`DZ_VERSION_STRING`，单一真相源根 CMakeLists.txt 的
 *   project(VERSION)），供「关于」页显示。前端不得另存硬编码版本（#207 A3）。
 *
 * 生命周期：App.vue onMounted 调用 init() 拉取一次；后端进程名与版本在运行期不变。
 */
export const useSystemStore = defineStore('system', () => {
  const processName = ref<string>('')
  const version = ref<string>('')
  const loaded = ref(false)

  async function init(): Promise<void> {
    if (loaded.value) return
    try {
      const info = await systemApi.info()
      processName.value = info.process_name
      version.value = info.version
    } catch {
      // 拉取失败时保持空串：tail 按钮仍可点击，后端 guard 兜底拒绝；
      // 关于页版本显示「未知」而不是硬编码版本
    } finally {
      loaded.value = true
    }
  }

  /** 判断指定 logger 是否为后端自身（用于禁用 tail 按钮 / 显示 WebUI 徽章） */
  function isSelf(logger: string): boolean {
    return !!processName.value && logger === processName.value
  }

  return { processName, version, loaded, init, isSelf }
})
