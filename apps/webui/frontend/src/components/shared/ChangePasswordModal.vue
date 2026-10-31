<script setup lang="ts">
import { ref, watch } from 'vue'
import Modal from './Modal.vue'
import { authApi } from '@/api/auth'
import { ApiError } from '@/api/client'
import { useAuthStore } from '@/stores/auth'
import {
  isPasswordAcceptable,
  MIN_PASSWORD_LENGTH,
  OLD_PASSWORD_INVALID_TEXT,
  PASSWORD_MISMATCH_TEXT,
  PASSWORD_TOO_WEAK_TEXT,
} from '@/composables/passwordPolicy'

// 自助改密表单（契约 rest §2.1/§2.2）：旧密码必填、新口令满足最小长度。
// 由侧边栏「修改密码」与强制改密弹窗（App.vue）共用，避免两套校验/文案漂移。
//
// 契约 rest §1「会话吊销」：改密成功后**当前会话也失效**（后端 token_version 自增 +
// 踢 WS），因此成功后一律 logout 并回登录页——不是可选的体验细节，而是协议要求。
const props = defineProps<{
  open: boolean
  /** 强制改密（管理员重置后的首次登录）：不可关闭、不可取消 */
  forced?: boolean
}>()

const emit = defineEmits<{
  close: []
  changed: []
}>()

const auth = useAuthStore()
const oldPassword = ref('')
const newPassword = ref('')
const confirmPassword = ref('')
const errorText = ref<string | null>(null)
const submitting = ref(false)

function reset(): void {
  oldPassword.value = ''
  newPassword.value = ''
  confirmPassword.value = ''
  errorText.value = null
  submitting.value = false
}

watch(() => props.open, (isOpen) => {
  if (isOpen) reset()
})

const tooShort = (): boolean => !isPasswordAcceptable(newPassword.value)

async function submit(): Promise<void> {
  errorText.value = null
  if (!oldPassword.value) {
    errorText.value = '请输入旧密码'
    return
  }
  if (tooShort()) {
    errorText.value = PASSWORD_TOO_WEAK_TEXT
    return
  }
  if (newPassword.value !== confirmPassword.value) {
    errorText.value = PASSWORD_MISMATCH_TEXT
    return
  }

  submitting.value = true
  try {
    await authApi.changePassword(oldPassword.value, newPassword.value)
    auth.clearMustChangePassword()
    emit('changed')
    // 契约 rest §1：既有 token 已吊销，必须重新登录
    auth.logout()
    await logoutToLogin()
  } catch (e: unknown) {
    // 契约 rest §1：错误体 code 是机器判别的唯一手段（password_too_weak /
    // invalid_old_password），不解析、不回显英文 error 文本
    errorText.value = mapErrorCode(e)
  } finally {
    submitting.value = false
  }
}

function mapErrorCode(e: unknown): string {
  if (e instanceof ApiError) {
    // body 可为 null（非 JSON 错误体）；ApiErrorBody 带索引签名，code 是 unknown
    const code: unknown = e.body?.code
    if (code === 'password_too_weak') return PASSWORD_TOO_WEAK_TEXT
    if (code === 'invalid_old_password') return OLD_PASSWORD_INVALID_TEXT
  }
  return '密码修改失败'
}

async function logoutToLogin(): Promise<void> {
  const { useRouter } = await import('vue-router')
  await useRouter().push('/login')
}

function close(): void {
  if (props.forced) return  // 强制改密时必须走「登出」，不能取消
  emit('close')
}
</script>

<template>
  <Modal :open="open" :title="forced ? '请修改密码' : '修改密码'" @close="close">
    <div class="dialog-form">
      <p
        v-if="forced"
        class="force-change-hint"
        data-test="force-change-hint"
      >管理员已重置你的密码，请设置新密码后重新登录。</p>
      <div class="dialog-field">
        <label class="dialog-field__label">旧密码</label>
        <div class="ds-input">
          <input
            v-model="oldPassword"
            type="password"
            autocomplete="current-password"
            placeholder="当前密码"
            data-test="old-password"
          >
        </div>
      </div>
      <div class="dialog-field">
        <label class="dialog-field__label">新密码</label>
        <div class="ds-input">
          <input
            v-model="newPassword"
            type="password"
            autocomplete="new-password"
            :placeholder="`至少 ${MIN_PASSWORD_LENGTH} 位`"
            data-test="new-password"
          >
        </div>
        <span class="dialog-field__hint" data-test="password-length-hint">密码长度至少 {{ MIN_PASSWORD_LENGTH }} 位</span>
      </div>
      <div class="dialog-field">
        <label class="dialog-field__label">确认新密码</label>
        <div class="ds-input">
          <input
            v-model="confirmPassword"
            type="password"
            autocomplete="new-password"
            placeholder="再次输入新密码"
            data-test="confirm-password"
          >
        </div>
      </div>
      <div v-if="errorText" class="change-password-error" role="alert">{{ errorText }}</div>
    </div>
    <template #footer>
      <button
        v-if="!forced"
        type="button"
        class="ds-btn ds-btn--secondary"
        @click="close"
      >取消</button>
      <button
        v-else
        type="button"
        class="ds-btn ds-btn--secondary"
        @click="auth.logout(); logoutToLogin()"
      >登出</button>
      <button
        type="button"
        class="ds-btn ds-btn--primary"
        :disabled="submitting"
        data-test="submit-password"
        @click="submit"
      >{{ submitting ? '提交中...' : '确认' }}</button>
    </template>
  </Modal>
</template>

<style scoped>
.dialog-field__hint {
  font-size: var(--body-xs-font-size);
  color: var(--text-tertiary);
}

.change-password-error {
  color: var(--status-error-default);
  font-size: var(--body-sm-font-size);
}

.force-change-hint {
  margin: 0;
  padding: var(--spacer-8) var(--spacer-12);
  border-radius: var(--radius-8);
  background: var(--status-warning-surface-l1);
  color: var(--text-default);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
}
</style>
