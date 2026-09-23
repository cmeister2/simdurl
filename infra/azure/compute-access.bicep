targetScope = 'resourceGroup'

param controllerPrincipalId string
param reaperPrincipalId string

resource controllerRole 'Microsoft.Authorization/roleDefinitions@2022-04-01' = {
  name: guid(resourceGroup().id, 'simdurl-controller-role')
  properties: {
    roleName: 'Simdurl benchmark controller ${uniqueString(resourceGroup().id)}'
    description: 'Manage disposable benchmark compute resources and ARM deployments.'
    type: 'CustomRole'
    assignableScopes: [resourceGroup().id]
    permissions: [{
      actions: [
        'Microsoft.Resources/deployments/*'
        'Microsoft.Resources/subscriptions/resourceGroups/read'
        'Microsoft.Compute/virtualMachines/*'
        'Microsoft.Compute/disks/*'
        'Microsoft.Network/networkInterfaces/*'
        'Microsoft.Network/publicIPAddresses/*'
        'Microsoft.Network/virtualNetworks/read'
        'Microsoft.Network/virtualNetworks/subnets/read'
        'Microsoft.Network/virtualNetworks/subnets/join/action'
      ]
    }]
  }
}

resource reaperRole 'Microsoft.Authorization/roleDefinitions@2022-04-01' = {
  name: guid(resourceGroup().id, 'simdurl-reaper-role')
  properties: {
    roleName: 'Simdurl benchmark reaper ${uniqueString(resourceGroup().id)}'
    description: 'Read, stop and delete registered disposable benchmark resources.'
    type: 'CustomRole'
    assignableScopes: [resourceGroup().id]
    permissions: [{
      actions: [
        'Microsoft.Resources/deployments/read'
        'Microsoft.Resources/deployments/cancel/action'
        'Microsoft.Compute/virtualMachines/read'
        'Microsoft.Compute/virtualMachines/delete'
        'Microsoft.Compute/virtualMachines/deallocate/action'
        'Microsoft.Compute/disks/read'
        'Microsoft.Compute/disks/delete'
        'Microsoft.Network/networkInterfaces/read'
        'Microsoft.Network/networkInterfaces/delete'
        'Microsoft.Network/publicIPAddresses/read'
        'Microsoft.Network/publicIPAddresses/delete'
      ]
    }]
  }
}

resource controllerAccess 'Microsoft.Authorization/roleAssignments@2022-04-01' = {
  name: guid(resourceGroup().id, controllerPrincipalId, controllerRole.id)
  properties: {
    principalId: controllerPrincipalId
    principalType: 'ServicePrincipal'
    roleDefinitionId: controllerRole.id
  }
}

resource reaperAccess 'Microsoft.Authorization/roleAssignments@2022-04-01' = {
  name: guid(resourceGroup().id, reaperPrincipalId, reaperRole.id)
  properties: {
    principalId: reaperPrincipalId
    principalType: 'ServicePrincipal'
    roleDefinitionId: reaperRole.id
  }
}
