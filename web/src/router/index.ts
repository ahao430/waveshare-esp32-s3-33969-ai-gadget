import { createRouter, createWebHistory } from 'vue-router'

export const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/', name: 'dashboard', component: () => import('@/pages/Dashboard.vue') },
    { path: '/appearance', name: 'appearance', component: () => import('@/pages/Appearance.vue') },
    { path: '/network', name: 'network', component: () => import('@/pages/Network.vue') },
    { path: '/proxy', name: 'proxy', component: () => import('@/pages/Proxy.vue') },
    { path: '/keys', name: 'keys', component: () => import('@/pages/Keys.vue') },
    { path: '/subscriptions', name: 'subscriptions', component: () => import('@/pages/Subscriptions.vue') },
    { path: '/bots', name: 'bots', component: () => import('@/pages/Bots.vue') },
    { path: '/voice', name: 'voice', component: () => import('@/pages/Voice.vue') },
    { path: '/muse', name: 'muse', component: () => import('@/pages/Muse.vue') },
    { path: '/xiaozhi', name: 'xiaozhi', component: () => import('@/pages/Xiaozhi.vue') },
    { path: '/zcode', name: 'zcode', component: () => import('@/pages/Zcode.vue') },
    { path: '/monitor', name: 'monitor', component: () => import('@/pages/Monitor.vue') },
    { path: '/weather', name: 'weather', component: () => import('@/pages/Weather.vue') },
  ],
})
