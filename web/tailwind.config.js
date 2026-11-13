/** @type {import('tailwindcss').Config} */
export default {
  content: ['./index.html', './src/**/*.{vue,ts}'],
  theme: {
    extend: {
      // 设计变量与 PLAN §8.1 / 模拟器 ui_theme.h 对齐
      colors: {
        bg: '#0b1020',
        side: '#0e1428',
        card: '#161d31',
        cardhi: '#1f2a47',
        line: '#212a44',
        accent: '#7c5cff',
        accent2: '#4f8cff',
        ok: '#3ddc84',
        warn: '#ffb13d',
        err: '#ff5d5d',
        txt: '#eaf0ff',
        dim: '#8a93ad',
      },
      fontFamily: {
        sans: [
          '-apple-system', 'BlinkMacSystemFont', 'Segoe UI', 'PingFang SC',
          'Hiragino Sans GB', 'Microsoft YaHei', 'sans-serif',
        ],
      },
    },
  },
  plugins: [],
}
