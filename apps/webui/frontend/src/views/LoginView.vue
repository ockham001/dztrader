<script setup lang="ts">
import { ref, computed, onMounted, nextTick } from 'vue'
import { useRouter } from 'vue-router'
import { useAuthStore } from '@/stores/auth'
import { ApiError } from '@/api/client'
import { loginErrorText, lockedRemainingMinutes } from '@/composables/loginErrors'
import ThemeDropdown from '@/components/layout/ThemeDropdown.vue'
import Icon from '@/components/shared/Icon.vue'

const router = useRouter()
const auth = useAuthStore()

// 回填上次登录用户名（登出或登录失败时保存的）
// fallback: token 过期场景下从 localStorage.user_info 读取
const initialUsername = (() => {
  try {
    return sessionStorage.getItem('last_username')
      || (JSON.parse(localStorage.getItem('user_info') || '{}')?.username ?? '')
  } catch { return '' }
})()
const username = ref(initialUsername)
const password = ref('')
const showPassword = ref(false)
const errorMessage = ref<string | null>(null)
const errorType = ref<'error' | 'warning' | null>(null)

const passwordIcon = computed(() => (showPassword.value ? 'EyeInvisible' : 'Eye'))

// 首屏聚焦用户名（#207 C5）：SPA 下 autofocus 属性在客户端路由挂载时不生效，
// 必须 onMounted 手动 focus（用 ref 而非 document.querySelector，组件可多实例/可测）。
const usernameInput = ref<HTMLInputElement | null>(null)
onMounted(async () => {
  await nextTick()
  usernameInput.value?.focus()
})

async function handleLogin(): Promise<void> {
  errorMessage.value = null
  errorType.value = null

  if (!username.value || !password.value) {
    errorMessage.value = '请输入用户名和密码'
    errorType.value = 'error'
    return
  }

  try {
    await auth.login({ username: username.value, password: password.value })
    // 登录成功，清除回填的用户名
    try { sessionStorage.removeItem('last_username') } catch { /* ignore */ }
    router.push('/')
  } catch (e: unknown) {
    // 登录失败，保留已输入的用户名供下次回填
    try { sessionStorage.setItem('last_username', username.value) } catch { /* ignore */ }
    if (e instanceof ApiError) {
      // 契约 rest §1/§2.1：登录失败错误体为 {"error": <machine_code>}（无 code 字段），
      // 按 error 机器码映射中文；account_locked 额外带绝对 Unix 秒级 locked_until。
      const body: unknown = e.body
      if (body && typeof body === 'object') {
        const record = body as Record<string, unknown>
        if (record.error === 'account_locked') {
          const minutes = lockedRemainingMinutes(record.locked_until)
          const text = loginErrorText(record.error)
          errorMessage.value = minutes > 0 ? `${text}，剩余 ${minutes} 分钟` : text
          errorType.value = 'warning'
        } else {
          // 未知/非法机器码由 loginErrorText 统一回落「登录失败」，绝不回显 snake_case
          errorMessage.value = loginErrorText(record.error)
          errorType.value = e.status >= 500 ? 'warning' : 'error'
        }
      } else {
        errorMessage.value = e.message || '登录失败'
        errorType.value = 'error'
      }
    } else if (e instanceof Error) {
      // Network error (fetch failed)
      errorMessage.value = '网络错误，请检查网络连接'
      errorType.value = 'warning'
    } else {
      errorMessage.value = '未知错误'
      errorType.value = 'error'
    }
  }
}

function togglePassword(): void {
  showPassword.value = !showPassword.value
}
</script>

<template>
  <!-- Theme dropdown (fixed top-right) -->
  <div :style="{ position: 'fixed', top: 'var(--spacer-16)', right: 'var(--spacer-16)', zIndex: 50 }">
    <ThemeDropdown />
  </div>

  <main :style="{ display: 'flex', alignItems: 'center', justifyContent: 'center', minHeight: '100vh', padding: 'var(--spacer-32) var(--spacer-16)', background: 'var(--bg-base-default)' }">
    <div :style="{ width: '100%', maxWidth: '384px' }">

      <!-- Brand Identity -->
      <div :style="{ textAlign: 'center', marginBottom: 'var(--spacer-20)' }">
        <div :style="{ marginBottom: 'var(--spacer-16)', display: 'inline-flex' }">
          <img
            src="/icons/builtin/DZ-Bolt.svg"
            alt="DZTrader"
            :style="{ width: '40px', height: '40px', borderRadius: '10px' }"
          />
        </div>
        <h1 :style="{
          fontFamily: 'var(--font-family-heading)',
          fontSize: 'var(--heading-xl-font-size)',
          lineHeight: 'var(--heading-xl-line-height)',
          fontWeight: 'var(--heading-xl-font-weight)',
          color: 'var(--text-default)',
          margin: 0,
        }">DZTrader 量化交易系统</h1>
      </div>

      <!-- Login Card -->
      <div class="ds-card" :style="{ padding: 'var(--spacer-24)' }">
        <form @submit.prevent="handleLogin" :style="{ display: 'flex', flexDirection: 'column', gap: 'var(--spacer-20)' }">

          <!-- Username Field -->
          <div :style="{ display: 'flex', flexDirection: 'column', gap: 'var(--spacer-8)' }">
            <label
              for="username"
              :style="{
                fontSize: 'var(--body-base-font-size)',
                lineHeight: 'var(--body-base-line-height)',
                color: 'var(--text-default)',
                fontWeight: 'var(--font-weight-medium)',
              }"
            >用户名</label>
            <div class="ds-input">
              <span class="ds-input__icon"><Icon name="UserCheck" :size="16" /></span>
              <input
                id="username"
                ref="usernameInput"
                v-model="username"
                type="text"
                placeholder="请输入用户名"
                autocomplete="username"
                required
                :disabled="auth.loading"
              />
            </div>
          </div>

          <!-- Password Field -->
          <div :style="{ display: 'flex', flexDirection: 'column', gap: 'var(--spacer-8)' }">
            <label
              for="password"
              :style="{
                fontSize: 'var(--body-base-font-size)',
                lineHeight: 'var(--body-base-line-height)',
                color: 'var(--text-default)',
                fontWeight: 'var(--font-weight-medium)',
              }"
            >密码</label>
            <div class="ds-input">
              <span class="ds-input__icon"><Icon name="Lock" :size="16" /></span>
              <input
                id="password"
                v-model="password"
                :type="showPassword ? 'text' : 'password'"
                placeholder="请输入密码"
                autocomplete="current-password"
                required
                :disabled="auth.loading"
              />
              <button
                type="button"
                :style="{
                  background: 'transparent',
                  border: 'none',
                  padding: 0,
                  width: '24px',
                  height: '24px',
                  minWidth: '24px',
                  display: 'inline-flex',
                  alignItems: 'center',
                  justifyContent: 'center',
                  cursor: 'pointer',
                  borderRadius: 'var(--radius-8)',
                  color: 'var(--icon-tertiary)',
                }"
                aria-label="显示密码"
                @click="togglePassword"
              >
                <Icon :name="passwordIcon" :size="16" />
              </button>
            </div>
          </div>

          <!-- Error message -->
          <div
            v-if="errorMessage"
            :style="{
              padding: 'var(--spacer-8) var(--spacer-12)',
              borderRadius: 'var(--radius-8)',
              fontSize: 'var(--body-sm-font-size)',
              lineHeight: 'var(--body-sm-line-height)',
              ...(errorType === 'error'
                ? { background: 'var(--status-error-surface-l1)', color: 'var(--status-error-default)' }
                : { background: 'var(--status-warning-surface-l1)', color: 'var(--status-warning-default)' }),
            }"
          >{{ errorMessage }}</div>

          <!-- Login Button -->
          <button
            type="submit"
            class="ds-btn ds-btn--brand ds-btn--lg"
            :disabled="auth.loading"
            :style="{ width: '100%', height: '38px', marginTop: 'var(--spacer-8)' }"
          >
            <span v-if="auth.loading" :style="{ display: 'inline-flex', alignItems: 'center', gap: 'var(--spacer-6)' }">
              <span :style="{
                width: '14px',
                height: '14px',
                border: '2px solid currentColor',
                borderTopColor: 'transparent',
                borderRadius: '50%',
                display: 'inline-block',
                animation: 'spin 0.6s linear infinite',
              }"></span>
              登录中...
            </span>
            <span v-else>登录</span>
          </button>

        </form>
      </div>

      <p :style="{
        textAlign: 'center',
        marginTop: 'var(--spacer-24)',
        fontSize: '12px',
        color: 'var(--text-tertiary)',
        letterSpacing: '0.5px',
      }">DZTrader Quantitative Trading System</p>

    </div>
  </main>
</template>

<style>
@keyframes spin {
  to { transform: rotate(360deg); }
}
</style>
