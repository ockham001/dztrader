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
      // 契约 rest §1/§2.1：登录失败错误体为 {"error": <human message>, "code": <machine code>}。
      // 前端只按 `code` 分支映射中文（`error` 是英文消息，回显给用户没有意义）；
      // account_locked 额外带绝对 Unix 秒级 locked_until。
      const record = (e.body ?? {}) as Record<string, unknown>
      if (record.code === 'account_locked') {
        const minutes = lockedRemainingMinutes(record.locked_until)
        const text = loginErrorText(record.code)
        errorMessage.value = minutes > 0 ? `${text}，剩余 ${minutes} 分钟` : text
        errorType.value = 'warning'
      } else {
        // 未知/缺失 code（含 400 bad request、网关错误体）统一「登录失败」，
        // 绝不回显 code 或英文 error 消息
        errorMessage.value = loginErrorText(record.code)
        errorType.value = e.status >= 500 ? 'warning' : 'error'
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
  <div class="login-theme-corner">
    <ThemeDropdown />
  </div>

  <main class="login-main">
    <div class="login-container">

      <!-- Brand Identity -->
      <div class="login-brand">
        <div class="login-brand__logo">
          <img
            src="/icons/builtin/DZ-Bolt.svg"
            alt="DZTrader"
            class="login-brand__logo-img"
          />
        </div>
        <h1 class="login-title">DZTrader 量化交易系统</h1>
      </div>

      <!-- Login Card -->
      <div class="ds-card login-card">
        <form @submit.prevent="handleLogin" class="login-form">

          <!-- Username Field -->
          <div class="login-field">
            <label for="username" class="login-field__label">用户名</label>
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
          <div class="login-field">
            <label for="password" class="login-field__label">密码</label>
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
                class="login-password-toggle"
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
            class="login-error"
            :class="{ 'login-error--warning': errorType !== 'error' }"
            :data-test-error-level="errorType"
          >{{ errorMessage }}</div>

          <!-- Login Button -->
          <button
            type="submit"
            class="ds-btn ds-btn--brand ds-btn--lg login-submit"
            :disabled="auth.loading"
          >
            <span v-if="auth.loading" class="login-submit__loading">
              <span class="login-submit__spinner"></span>
              登录中...
            </span>
            <span v-else>登录</span>
          </button>

        </form>
      </div>

      <p class="login-footer">DZTrader Quantitative Trading System</p>

    </div>
  </main>
</template>

<style>
@keyframes spin {
  to { transform: rotate(360deg); }
}
</style>

<style scoped>
/* #346：模板里静态的 :style 声明归位到本组件的 scoped 样式；值逐字搬运，未做任何视觉调整。
   @keyframes spin 仍留在上面的非 scoped 块（原本就全局，不随本风格改动）。 */
.login-theme-corner {
  position: fixed;
  top: var(--spacer-16);
  right: var(--spacer-16);
  z-index: 50;
}

.login-main {
  display: flex;
  align-items: center;
  justify-content: center;
  min-height: 100vh;
  padding: var(--spacer-32) var(--spacer-16);
  background: var(--bg-base-default);
}

.login-container {
  width: 100%;
  max-width: 384px;
}

.login-brand {
  text-align: center;
  margin-bottom: var(--spacer-20);
}

.login-brand__logo {
  margin-bottom: var(--spacer-16);
  display: inline-flex;
}

.login-brand__logo-img {
  width: 40px;
  height: 40px;
  border-radius: var(--radius-10);
}

.login-title {
  font-family: var(--font-family-heading);
  font-size: var(--heading-xl-font-size);
  line-height: var(--heading-xl-line-height);
  font-weight: var(--heading-xl-font-weight);
  color: var(--text-default);
  margin: 0;
}

.login-card {
  padding: var(--spacer-24);
}

.login-form {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-20);
}

.login-field {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-8);
}

.login-field__label {
  font-size: var(--body-base-font-size);
  line-height: var(--body-base-line-height);
  color: var(--text-default);
  font-weight: var(--font-weight-medium);
}

.login-password-toggle {
  background: transparent;
  border: none;
  padding: 0;
  width: 24px;
  height: 24px;
  min-width: 24px;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  cursor: pointer;
  border-radius: var(--radius-8);
  color: var(--icon-tertiary);
}

/* 底色/文字色随 errorType 变化，属逐实例动态值，仍由模板内联三元决定，这里只放静态部分。 */
.login-error {
  padding: var(--spacer-8) var(--spacer-12);
  border-radius: var(--radius-8);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  /* 默认（errorType === 'error'）走错误色；警告级别见下面的修饰类 */
  background: var(--status-error-surface-l1);
  color: var(--status-error-default);
}

/* 警告级提示（如「密码即将过期」）。原先是内联 spread 三元：两个分支都是静态字面量，
   只是按 errorType 选配色 —— 与 SyncSwitch/UserManagement 的同类处理一致，提为修饰类。 */
.login-error--warning {
  background: var(--status-warning-surface-l1);
  color: var(--status-warning-default);
}

.login-submit {
  width: 100%;
  height: 38px;
  margin-top: var(--spacer-8);
}

.login-submit__loading {
  display: inline-flex;
  align-items: center;
  gap: var(--spacer-6);
}

.login-submit__spinner {
  width: 14px;
  height: 14px;
  border: 2px solid currentColor;
  border-top-color: transparent;
  border-radius: 50%;
  display: inline-block;
  animation: spin 0.6s linear infinite;
}

.login-footer {
  text-align: center;
  margin-top: var(--spacer-24);
  font-size: var(--body-sm-font-size);
  color: var(--text-tertiary);
  letter-spacing: 0.5px;
}
</style>
