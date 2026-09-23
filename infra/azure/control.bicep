targetScope = 'resourceGroup'

param location string = resourceGroup().location
param computeResourceGroup string
@description('GitHub owner/repository whose main branch can use the benchmark environment.')
param githubRepository string
@description('Actual repository OIDC subject prefix, including immutable owner/repository IDs when enabled.')
param githubSubjectPrefix string = 'repo:${githubRepository}'
param githubEnvironment string = 'Benchmarking'
@description('Existing Entra application client ID. Supply together with externalControllerPrincipalId, or leave both empty to create a managed identity.')
param externalControllerClientId string = ''
@description('Service principal object ID for the existing controller application, not its application object ID. External federation remains owned by that application.')
param externalControllerPrincipalId string = ''
@description('Globally unique lowercase alphanumeric name, 3 to 24 characters.')
@minLength(3)
@maxLength(24)
param storageAccountName string
param functionAppName string = 'simdurl-reaper-${uniqueString(resourceGroup().id)}'
@description('Optional alert destination; configure before live qualification.')
param alertEmail string = ''

var blobContributor = subscriptionResourceId('Microsoft.Authorization/roleDefinitions', 'ba92f5b4-2d11-453d-a403-e96b0029c9fe')
var blobReader = subscriptionResourceId('Microsoft.Authorization/roleDefinitions', '2a2b9908-6ea1-4ae2-8e65-a410df84e7d1')
var blobOwner = subscriptionResourceId('Microsoft.Authorization/roleDefinitions', 'b7e6dc6d-f1e8-4753-8033-0f276bb0955b')
var blobDelegator = subscriptionResourceId('Microsoft.Authorization/roleDefinitions', 'db58b8e5-c6ad-4a2a-8342-4190687cbf4a')
var queueContributor = subscriptionResourceId('Microsoft.Authorization/roleDefinitions', '974c5e8b-45b9-4653-ba55-5f855dd0fb88')
var monitorPublisher = subscriptionResourceId('Microsoft.Authorization/roleDefinitions', '3913510d-42f4-4e42-8a64-420c390055eb')
var containerNames = ['inputs', 'results', 'runs', 'state']
var useExternalController = empty(externalControllerClientId) == empty(externalControllerPrincipalId)
  ? !empty(externalControllerClientId)
  : fail('externalControllerClientId and externalControllerPrincipalId must either both be supplied or both be empty.')
var controllerPrincipalId = useExternalController ? externalControllerPrincipalId : controllerIdentity!.properties.principalId
var controllerClientId = useExternalController ? externalControllerClientId : controllerIdentity!.properties.clientId
// Preserve the existing GUID seeds for default managed-identity deployments.
var controllerRoleAssignmentIdentity = useExternalController
  ? externalControllerPrincipalId
  : resourceId('Microsoft.ManagedIdentity/userAssignedIdentities', 'simdurl-bench-controller')

resource controllerIdentity 'Microsoft.ManagedIdentity/userAssignedIdentities@2023-01-31' = if (!useExternalController) {
  name: 'simdurl-bench-controller'
  location: location
}
resource githubIdentity 'Microsoft.ManagedIdentity/userAssignedIdentities/federatedIdentityCredentials@2023-01-31' = if (!useExternalController) {
  parent: controllerIdentity
  name: 'github-benchmarks'
  properties: {
    issuer: 'https://token.actions.githubusercontent.com'
    subject: '${githubSubjectPrefix}:environment:${githubEnvironment}'
    audiences: ['api://AzureADTokenExchange']
  }
}
resource reaperIdentity 'Microsoft.ManagedIdentity/userAssignedIdentities@2023-01-31' = {
  name: 'simdurl-bench-reaper'
  location: location
}

resource storage 'Microsoft.Storage/storageAccounts@2023-05-01' = {
  name: storageAccountName
  location: location
  kind: 'StorageV2'
  sku: { name: 'Standard_LRS' }
  properties: {
    minimumTlsVersion: 'TLS1_2'
    supportsHttpsTrafficOnly: true
    allowBlobPublicAccess: false
    allowSharedKeyAccess: false
    publicNetworkAccess: 'Enabled'
  }
}
resource blobService 'Microsoft.Storage/storageAccounts/blobServices@2023-05-01' = {
  parent: storage
  name: 'default'
}
resource containers 'Microsoft.Storage/storageAccounts/blobServices/containers@2023-05-01' = [for containerName in containerNames: {
  parent: blobService
  name: containerName
  properties: { publicAccess: 'None' }
}]
resource retention 'Microsoft.Storage/storageAccounts/managementPolicies@2023-05-01' = {
  parent: storage
  name: 'default'
  properties: {
    policy: {
      rules: [
        {
          name: 'expire-inputs'
          enabled: true
          type: 'Lifecycle'
          definition: {
            filters: { blobTypes: ['blockBlob'], prefixMatch: ['inputs/'] }
            actions: { baseBlob: { delete: { daysAfterModificationGreaterThan: 1 } } }
          }
        }
        {
          name: 'expire-evidence'
          enabled: true
          type: 'Lifecycle'
          definition: {
            filters: { blobTypes: ['blockBlob', 'appendBlob'], prefixMatch: ['results/'] }
            actions: { baseBlob: { delete: { daysAfterModificationGreaterThan: 90 } } }
          }
        }
      ]
    }
  }
}
resource controllerBlobs 'Microsoft.Authorization/roleAssignments@2022-04-01' = [for (containerName, i) in containerNames: {
  name: guid(containers[i].id, controllerRoleAssignmentIdentity, blobContributor)
  scope: containers[i]
  properties: { principalId: controllerPrincipalId, principalType: 'ServicePrincipal', roleDefinitionId: blobContributor }
}]
resource controllerDelegation 'Microsoft.Authorization/roleAssignments@2022-04-01' = {
  name: guid(storage.id, controllerRoleAssignmentIdentity, blobDelegator)
  scope: storage
  properties: { principalId: controllerPrincipalId, principalType: 'ServicePrincipal', roleDefinitionId: blobDelegator }
}
resource reaperManifestAccess 'Microsoft.Authorization/roleAssignments@2022-04-01' = {
  name: guid(containers[2].id, reaperIdentity.id, blobReader)
  scope: containers[2]
  properties: { principalId: reaperIdentity.properties.principalId, principalType: 'ServicePrincipal', roleDefinitionId: blobReader }
}
resource reaperStateAccess 'Microsoft.Authorization/roleAssignments@2022-04-01' = {
  name: guid(containers[3].id, reaperIdentity.id, blobContributor)
  scope: containers[3]
  properties: { principalId: reaperIdentity.properties.principalId, principalType: 'ServicePrincipal', roleDefinitionId: blobContributor }
}

// Isolate Functions host/package storage from controller-writable artifacts.
resource hostStorage 'Microsoft.Storage/storageAccounts@2023-05-01' = {
  name: 'simdfn${uniqueString(resourceGroup().id)}'
  location: location
  kind: 'StorageV2'
  sku: { name: 'Standard_LRS' }
  properties: {
    minimumTlsVersion: 'TLS1_2'
    supportsHttpsTrafficOnly: true
    allowBlobPublicAccess: false
    allowSharedKeyAccess: false
  }
}
resource hostBlobService 'Microsoft.Storage/storageAccounts/blobServices@2023-05-01' = { parent: hostStorage, name: 'default' }
resource packages 'Microsoft.Storage/storageAccounts/blobServices/containers@2023-05-01' = {
  parent: hostBlobService
  name: 'reaper-package'
  properties: { publicAccess: 'None' }
}
resource hostBlobAccess 'Microsoft.Authorization/roleAssignments@2022-04-01' = {
  name: guid(hostStorage.id, reaperIdentity.id, blobOwner)
  scope: hostStorage
  properties: { principalId: reaperIdentity.properties.principalId, principalType: 'ServicePrincipal', roleDefinitionId: blobOwner }
}
resource hostQueueAccess 'Microsoft.Authorization/roleAssignments@2022-04-01' = {
  name: guid(hostStorage.id, reaperIdentity.id, queueContributor)
  scope: hostStorage
  properties: { principalId: reaperIdentity.properties.principalId, principalType: 'ServicePrincipal', roleDefinitionId: queueContributor }
}
resource workspace 'Microsoft.OperationalInsights/workspaces@2023-09-01' = {
  name: '${functionAppName}-logs'
  location: location
  properties: { sku: { name: 'PerGB2018' }, retentionInDays: 30, workspaceCapping: { dailyQuotaGb: json('0.1') } }
}
resource insights 'Microsoft.Insights/components@2020-02-02' = {
  name: functionAppName
  location: location
  kind: 'web'
  properties: { Application_Type: 'web', WorkspaceResourceId: workspace.id, DisableLocalAuth: true }
}
resource monitorAccess 'Microsoft.Authorization/roleAssignments@2022-04-01' = {
  name: guid(insights.id, reaperIdentity.id, monitorPublisher)
  scope: insights
  properties: { principalId: reaperIdentity.properties.principalId, principalType: 'ServicePrincipal', roleDefinitionId: monitorPublisher }
}
resource plan 'Microsoft.Web/serverfarms@2024-04-01' = {
  name: '${functionAppName}-plan'
  location: location
  kind: 'functionapp'
  sku: { name: 'FC1', tier: 'FlexConsumption' }
  properties: { reserved: true }
}
resource functionApp 'Microsoft.Web/sites@2024-04-01' = {
  name: functionAppName
  location: location
  kind: 'functionapp,linux'
  identity: { type: 'UserAssigned', userAssignedIdentities: { '${reaperIdentity.id}': {} } }
  properties: {
    serverFarmId: plan.id
    httpsOnly: true
    functionAppConfig: {
      runtime: { name: 'python', version: '3.12' }
      deployment: {
        storage: {
          type: 'blobContainer'
          value: '${hostStorage.properties.primaryEndpoints.blob}${packages.name}'
          authentication: { type: 'UserAssignedIdentity', userAssignedIdentityResourceId: reaperIdentity.id }
        }
      }
      scaleAndConcurrency: { maximumInstanceCount: 40, instanceMemoryMB: 512 }
    }
    siteConfig: {
      minTlsVersion: '1.2'
      appSettings: [
        { name: 'AzureWebJobsStorage__accountName', value: hostStorage.name }
        { name: 'AzureWebJobsStorage__credential', value: 'managedidentity' }
        { name: 'AzureWebJobsStorage__clientId', value: reaperIdentity.properties.clientId }
        { name: 'AZURE_CLIENT_ID', value: reaperIdentity.properties.clientId }
        { name: 'CONTROL_ACCOUNT_URL', value: storage.properties.primaryEndpoints.blob }
        { name: 'COMPUTE_SUBSCRIPTION_ID', value: subscription().subscriptionId }
        { name: 'COMPUTE_RESOURCE_GROUP', value: computeResourceGroup }
        { name: 'APPLICATIONINSIGHTS_CONNECTION_STRING', value: insights.properties.ConnectionString }
        { name: 'APPLICATIONINSIGHTS_AUTHENTICATION_STRING', value: 'ClientId=${reaperIdentity.properties.clientId};Authorization=AAD' }
      ]
    }
  }
  dependsOn: [hostBlobAccess, hostQueueAccess]
}

module computeAccess 'compute-access.bicep' = {
  name: 'simdurl-compute-access'
  scope: resourceGroup(computeResourceGroup)
  params: {
    controllerPrincipalId: controllerPrincipalId
    reaperPrincipalId: reaperIdentity.properties.principalId
  }
}

resource alerts 'Microsoft.Insights/actionGroups@2023-01-01' = if (!empty(alertEmail)) {
  name: 'simdurl-benchmark-alerts'
  location: 'global'
  properties: {
    groupShortName: 'simdurl'
    enabled: true
    emailReceivers: [{ name: 'benchmark-owner', emailAddress: alertEmail, useCommonAlertSchema: true }]
  }
}
resource reaperAlert 'Microsoft.Insights/scheduledQueryRules@2023-12-01' = if (!empty(alertEmail)) {
  name: 'simdurl-reaper-health'
  location: location
  properties: {
    displayName: 'Simdurl cleanup failed or heartbeat missing'
    enabled: true
    severity: 1
    evaluationFrequency: 'PT5M'
    windowSize: 'PT15M'
    scopes: [workspace.id]
    criteria: {
      allOf: [{
        query: 'AppTraces | where TimeGenerated > ago(15m) | summarize heartbeats=countif(Message == "simdurl-reaper-heartbeat"), failures=countif(Message startswith "simdurl-reaper-error") | where heartbeats == 0 or failures > 0'
        timeAggregation: 'Count'
        operator: 'GreaterThan'
        threshold: 0
        failingPeriods: { numberOfEvaluationPeriods: 1, minFailingPeriodsToAlert: 1 }
      }]
    }
    actions: { actionGroups: [alerts!.id] }
  }
}

output storageAccount string = storage.name
output functionName string = functionApp.name
output controllerClientId string = controllerClientId
output controllerPrincipalId string = controllerPrincipalId
output tenantId string = tenant().tenantId
output subscriptionId string = subscription().subscriptionId
output githubSubject string = '${githubSubjectPrefix}:environment:${githubEnvironment}'
