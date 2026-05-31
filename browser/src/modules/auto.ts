import { useMenuStore } from 'spangap-browser/stores/menu'
import AutoPanel from '../panels/AutoPanel.vue'

export function registerAuto() {
  useMenuStore().register('settings', 'Settings', [
    {
      id: 'reticulum', label: 'Reticulum', type: 'submenu',
      children: [
        {
          id: 'reticulum.transports', label: 'Transports', type: 'submenu',
          children: [
            { id: 'reticulum.transports.auto', label: 'AutoInterface', type: 'panel',
              component: AutoPanel },
          ],
        },
      ],
    },
  ])
}
