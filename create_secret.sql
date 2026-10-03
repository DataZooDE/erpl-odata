CREATE PERSISTENT SECRET datasphere (
    TYPE 'datasphere', 
    provider 'oauth2', 
    client_id '<your-client-id>',
    client_secret '<your-client-secret>', 
    tenant_name '<your-tenant>', 
    data_center 'eu10', 
    scope 'openid uaa.user', 
    redirect_uri 'http://localhost:65000'
);
