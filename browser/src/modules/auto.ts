import { useMenuStore } from 'spangap-browser/stores/menu'
import AutoPanel from '../panels/AutoPanel.vue'

export function registerAuto() {
  const menu = useMenuStore()
  menu.setMenu('settings/mesh/interfaces', { label: 'RNS Interfaces', placement: 2 })
  menu.register('settings/mesh/interfaces/auto', 'AutoInterface', { type: 'panel', component: AutoPanel })
}
