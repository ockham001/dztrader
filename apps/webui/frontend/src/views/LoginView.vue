<script setup lang="ts">
import { ref, computed } from 'vue'
import { useRouter } from 'vue-router'
import { useAuthStore } from '@/stores/auth'
import { ApiError } from '@/api/client'
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
      const body = e.body
      if (body && typeof body === 'object') {
        const code = 'code' in body ? String(body.code) : undefined
        const error = 'error' in body ? String(body.error) : '登录失败'

        if (code === 'account_locked' && 'locked_until' in body) {
          const lockedUntil = Number(body.locked_until)
          if (lockedUntil > 0) {
            const remainingSec = Math.max(0, lockedUntil - Math.floor(Date.now() / 1000))
            const minutes = Math.ceil(remainingSec / 60)
            errorMessage.value = `${error}，剩余 ${minutes} 分钟`
          } else {
            errorMessage.value = error
          }
          errorType.value = 'warning'
        } else if (code === 'ip_banned') {
          errorMessage.value = error
          errorType.value = 'error'
        } else if (code === 'account_disabled') {
          errorMessage.value = error
          errorType.value = 'error'
        } else if (code === 'invalid_credentials') {
          errorMessage.value = error
          errorType.value = 'error'
        } else {
          errorMessage.value = error
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
                fontWeight: 'var(--font-weight-medium, 500)',
              }"
            >用户名</label>
            <div class="ds-input">
              <span class="ds-input__icon"><Icon name="UserCheck" :size="16" /></span>
              <input
                id="username"
                v-model="username"
                type="text"
                placeholder="请输入用户名"
                autocomplete="username"
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
                fontWeight: 'var(--font-weight-medium, 500)',
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
