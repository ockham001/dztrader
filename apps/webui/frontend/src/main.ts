import { createApp } from 'vue'
import { createPinia } from 'pinia'
import App from './App.vue'
import router from './router'
import './composables/wsHandlers'
import './assets/css/design-tokens.css'
import './assets/css/theme.css'
import './assets/css/components.css'
import './assets/css/layout.css'
import './assets/css/dashboard.css'

const app = createApp(App)
app.use(createPinia())
app.use(router)
app.mount('#app')
