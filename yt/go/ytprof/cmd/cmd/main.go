package main

import (
	"fmt"
	"os"

	"github.com/spf13/cobra"

	"a.yandex-team.ru/yt/go/yt"
	"a.yandex-team.ru/yt/go/yt/ythttp"
)

var (
	flagFilePath     string
	flagProxy        string
	flagTablePath    string
	flagMetaquery    string
	flagIgnoreErrors bool
	flagTimestampMin string
	flagTimestampMax string
	flagTimeLast     string
	flagStats        bool
	flagStatsMax     int
	flagQueryLimit   int

	YT yt.Client
)

var rootCmd = &cobra.Command{
	Use: "ytprof",

	PersistentPreRunE: func(cmd *cobra.Command, args []string) error {
		var err error
		YT, err = ythttp.NewClient(&yt.Config{
			Proxy:             flagProxy,
			ReadTokenFromFile: true,
		})
		return err
	},
}

func init() {
	rootCmd.PersistentFlags().StringVar(&flagProxy, "storage-proxy", "hahn", "name of the YT cluster")
	rootCmd.PersistentFlags().StringVar(&flagTablePath, "storage-path", "//home/ytprof/storage/manual", "path to directory with table data & metadata")
	rootCmd.PersistentFlags().StringVar(&flagMetaquery, "metaquery", "true",
		`query to metadata like 
		("Metadata['BinaryVersion'] == '22.1.9091469-stable-ya~42704c91e804aabc'" or
		"Metadata['BinaryVersion'].matches('^22.3.*stable')")`)
	rootCmd.PersistentFlags().BoolVar(&flagIgnoreErrors, "ignore-errors", false, "considers errors in metaquery evaluation not a match")
	rootCmd.PersistentFlags().StringVar(&flagTimestampMin, "mintime", "", "start of the time period")
	rootCmd.PersistentFlags().StringVar(&flagTimestampMax, "maxtime", "", "end of the time period")
	rootCmd.PersistentFlags().StringVar(&flagTimeLast, "last", "", "alternate way to to describe period: [now-last, now]")
	rootCmd.PersistentFlags().StringVar(&flagFilePath, "file", "", "file to save profiles in")
	rootCmd.PersistentFlags().BoolVar(&flagStats, "stats", false, "option to display found stats")
	rootCmd.PersistentFlags().IntVar(&flagStatsMax, "statsmax", 5, "maximum length of metadata parm values list to display")
	rootCmd.PersistentFlags().IntVar(&flagQueryLimit, "limit", 50000, "limit of rows processed in query")
}

func Execute() {
	if err := rootCmd.Execute(); err != nil {
		_, _ = fmt.Fprintf(os.Stderr, "%+v\n", err)
		os.Exit(1)
	}
}

func main() {
	Execute()
}
