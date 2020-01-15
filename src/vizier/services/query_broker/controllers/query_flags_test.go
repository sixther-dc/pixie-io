package controllers_test

import (
	"testing"

	"github.com/stretchr/testify/assert"
	"pixielabs.ai/pixielabs/src/vizier/services/query_broker/controllers"
)

const validQueryWithFlag = `
#pl:set analyze=true

t1 = dataframe(table='http_events').range(start='-30s')

t1['service'] = t1.ctx['service']
t1['http_resp_latency_ms'] = t1['http_resp_latency_ns'] / 1.0E6
t1['failure'] = t1['http_resp_status'] >= 400
t1['range_group'] = pl.subtract(t1['time_'], pl.modulo(t1['time_'], 1000000000))
`

const validQueryWithoutFlag = `
t1 = dataframe(table='http_events').range(start='-30s')

t1['service'] = t1.ctx['service']
t1['http_resp_latency_ms'] = t1['http_resp_latency_ns'] / 1.0E6
t1['failure'] = t1['http_resp_status'] >= 400
t1['range_group'] = pl.subtract(t1['time_'], pl.modulo(t1['time_'], 1000000000))
`

const invalidFlag1 = `
#pl:set analyze=true extra
`

const invalidFlag2 = `
#pl:set analyze,true
`

const nonexistentFlag = `
#pl:set ABCD=efgh
`

func TestParseQueryFlags_WithFlag(t *testing.T) {
	qf, err := controllers.ParseQueryFlags(validQueryWithFlag)

	assert.Nil(t, err)
	assert.NotNil(t, qf)

	val := qf.GetBool("analyze")
	assert.Equal(t, true, val)

	val = qf.GetBool("invalid_key")
	assert.Equal(t, false, val)
}

func TestParseQueryFlags_NoFlag(t *testing.T) {
	qf, err := controllers.ParseQueryFlags(validQueryWithoutFlag)

	assert.Nil(t, err)
	assert.NotNil(t, qf)

	val := qf.GetBool("analyze")
	assert.Equal(t, false, val)

	val = qf.GetBool("invalid_key")
	assert.Equal(t, false, val)
}

func TestParseQueryFlags_InvalidFlag(t *testing.T) {
	qf, err := controllers.ParseQueryFlags(invalidFlag1)
	assert.Nil(t, qf)
	assert.NotNil(t, err)

	qf, err = controllers.ParseQueryFlags(invalidFlag2)
	assert.Nil(t, qf)
	assert.NotNil(t, err)

	qf, err = controllers.ParseQueryFlags(nonexistentFlag)
	assert.Nil(t, qf)
	assert.NotNil(t, err)
}

func TestParseQueryFlags_PlanOptions(t *testing.T) {
	qf, err := controllers.ParseQueryFlags(validQueryWithFlag)

	assert.Nil(t, err)
	assert.NotNil(t, qf)

	options := qf.GetPlanOptions()
	assert.Equal(t, options.Explain, false)
	assert.Equal(t, options.Analyze, true)
}
