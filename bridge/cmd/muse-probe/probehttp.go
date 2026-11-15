// probehttp.go — muse-probe 的最小 HTTP 帮助（带代理）。
package main

import (
	"fmt"
	"io"
	"net/http"
	"net/url"
	"time"
)

func httpGetJSON(proxyURL, target, token string) ([]byte, error) {
	tr := &http.Transport{}
	if proxyURL != "" {
		pu, err := url.Parse(proxyURL)
		if err != nil {
			return nil, fmt.Errorf("proxy: %w", err)
		}
		tr.Proxy = http.ProxyURL(pu)
	} else {
		tr.Proxy = http.ProxyFromEnvironment
	}
	req, err := http.NewRequest(http.MethodGet, target, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Authorization", "Bearer "+token)
	client := &http.Client{Transport: tr, Timeout: 30 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("http %d: %.200s", resp.StatusCode, string(body))
	}
	return body, nil
}
