import { createRouter, createWebHistory } from 'vue-router'

export const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/', name: 'dashboard', component: () => import('@/pages/Dashboard.vue') },
    { path: '/subscriptions', name: 'subscriptions', component: () => import('@/pages/Subscriptions.vue') },
    { path: '/network', name: 'network', component: () => import('@/pages/Network.vue') },
    { path: '/provisioning', name: 'provisioning', component: () => import('@/pages/Provisioning.vue') },
    { path: '/chat', name: 'chat', component: () => import('@/pages/Chat.vue') },
    { path: '/monitoring', name: 'monitoring', component: () => import('@/pages/Monitoring.vue') },
    { path: '/appearance', name: 'appearance', component: () => import('@/pages/Appearance.vue') },
  ],
})
