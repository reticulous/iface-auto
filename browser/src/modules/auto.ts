import { useMenuStore } from 'spangap-browser/stores/menu'
import AutoPanel from '../panels/AutoPanel.vue'

export function registerAuto() {
  useMenuStore().register('settings/reticulum/transports/auto', 'AutoInterface', { type: 'panel', component: AutoPanel })
}
