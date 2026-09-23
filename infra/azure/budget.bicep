targetScope = 'subscription'

param budgetName string = 'simdurl-benchmark-monthly'
@description('Resource group containing all transient benchmark VMs and their network and disk resources.')
@minLength(1)
param computeResourceGroup string
@description('Resource group containing benchmark storage, the reaper, and monitoring resources.')
@minLength(1)
param controlResourceGroup string
@description('Monthly alert budget in the subscription billing currency. Verify that currency before treating this amount as USD. This budget does not stop resource consumption.')
@minValue(1)
param amount int = 100
@description('First day of the current calendar month in UTC, for example 2026-09-01T00:00:00Z. Azure defaults the end date to ten years after this date.')
param startDate string
@description('Email address receiving actual and forecast budget alerts.')
@minLength(3)
param contactEmail string
@description('Optional controller service principal object ID. Grants read access only to this budget, for allocation admission checks.')
param controllerPrincipalId string = ''

var coveredResourceGroups = toLower(computeResourceGroup) != toLower(controlResourceGroup)
  ? [computeResourceGroup, controlResourceGroup]
  : fail('computeResourceGroup and controlResourceGroup must identify two distinct resource groups.')
var readerRole = subscriptionResourceId('Microsoft.Authorization/roleDefinitions', 'acdd72a7-3385-48ef-bd42-f606fba81ae7')

resource monthlyBudget 'Microsoft.Consumption/budgets@2024-08-01' = {
  name: budgetName
  properties: {
    amount: amount
    category: 'Cost'
    filter: {
      dimensions: {
        name: 'ResourceGroupName'
        operator: 'In'
        values: coveredResourceGroups
      }
    }
    notifications: {
      Actual80: {
        enabled: true
        operator: 'GreaterThanOrEqualTo'
        threshold: 80
        thresholdType: 'Actual'
        contactEmails: [contactEmail]
        locale: 'en-us'
      }
      Actual100: {
        enabled: true
        operator: 'GreaterThanOrEqualTo'
        threshold: 100
        thresholdType: 'Actual'
        contactEmails: [contactEmail]
        locale: 'en-us'
      }
      Forecasted100: {
        enabled: true
        operator: 'GreaterThanOrEqualTo'
        threshold: 100
        thresholdType: 'Forecasted'
        contactEmails: [contactEmail]
        locale: 'en-us'
      }
    }
    timeGrain: 'Monthly'
    timePeriod: {
      startDate: startDate
    }
  }
}

resource controllerBudgetReader 'Microsoft.Authorization/roleAssignments@2022-04-01' = if (!empty(controllerPrincipalId)) {
  scope: monthlyBudget
  name: guid(monthlyBudget.id, controllerPrincipalId, readerRole)
  properties: {
    principalId: controllerPrincipalId
    principalType: 'ServicePrincipal'
    roleDefinitionId: readerRole
  }
}

output budgetId string = monthlyBudget.id
output budgetAmount int = amount
output resourceGroups array = coveredResourceGroups
